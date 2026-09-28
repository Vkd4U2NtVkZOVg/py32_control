/**
 * @file nv_params.c
 * @brief Non-volatile parameter storage over the internal FLASH.
 *
 * Implements a single-block parameter store in the last FLASH sector
 * (page 0x0800F000). The block is one 128-byte flash page:
 *
 *     [ magic 4B ][ version 4B ][ crc32 4B ][ params[29] 116B ]
 *
 * On power-up NvParams_Init() copies the stored block into the RAM cache after
 * validating magic/version/crc; on any mismatch it fills the cache with the
 * firmware defaults. NvParams_Save() erases the page and programms the whole
 * block again (single-block direct write, ~10k cycle endurance).
 *
 * All flash access goes through the HAL wrappers (HAL_FLASH_Erase /
 * HAL_FLASH_Program), which configure the 32 MHz program/erase timing
 * internally and wait for completion. IRQs are disabled around the flash
 * access so the saved block is not torn by an interrupt.
 */
#include "nv_params.h"
#include "py32f0xx_hal_flash.h"
#include "main.h"

#include <string.h>   /* memcpy */

/*============================================================================*/
/* Config                                                                      */
/*============================================================================*/
#define NVM_BASE        (0x0800F000UL)  /**< last flash sector, page-aligned  */
#define NVM_MAGIC       (0x4E564432u)   /**< 'NVD2' - non-volatile data v2     */
#define NVM_VERSION     (1u)

#define NVM_OK          (0)
#define NVM_ERR_PARAM   (-1)
#define NVM_ERR_CRC     (-2)
#define NVM_ERR_FLASH   (-3)

/*============================================================================*/
/* On-flash block layout (must be exactly one 128-byte page, page-aligned)     */
/*============================================================================*/
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc;                       /**< CRC-32 over params[]             */
    uint32_t params[ NVM_PARAM_COUNT ];
} Nvm_Block_t;
_Static_assert( sizeof( Nvm_Block_t ) == 128, "Nvm_Block_t must be one flash page" );

/*============================================================================*/
/* RAM cache + defaults                                                        */
/*============================================================================*/
static uint32_t s_cache[ NVM_PARAM_COUNT ];

/* Firmware default values. Override with actual production defaults. */
static const uint32_t s_defaults[ NVM_PARAM_COUNT ] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
};

/*============================================================================*/
/* CRC-32 (reflected ISO-HDLC, poly 0xEDB88320) over a byte buffer. Check      */
/* value: crc32("123456789") == 0xCBF43926.                                     */
/*============================================================================*/
static uint32_t nv_crc32( const uint8_t *data, uint32_t length )
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    uint8_t bit;

    for ( i = 0u ; i < length ; ++i ) {
        crc ^= data[ i ];
        for ( bit = 0u ; bit < 8u ; ++bit ) {
            crc = ( crc & 1u ) ? ( crc >> 1u ) ^ 0xEDB88320u : ( crc >> 1u );
        }
    }
    return ~crc;
}

static void nv_load_defaults( void )
{
    memcpy( s_cache, s_defaults, sizeof( s_cache ) );
}

/*============================================================================*/
/* Public API                                                                  */
/*============================================================================*/
void NvParams_Init( void )
{
    const Nvm_Block_t *stored = (const Nvm_Block_t *)NVM_BASE;

    if ( ( stored->magic == NVM_MAGIC ) &&
         ( stored->version == NVM_VERSION ) &&
         ( stored->crc == nv_crc32( (const uint8_t *)stored->params,
                                    sizeof( stored->params ) ) ) ) {
        memcpy( s_cache, stored->params, sizeof( s_cache ) );
    } else {
        nv_load_defaults();
    }
}

uint32_t NvParams_Get( uint8_t id )
{
    if ( id >= NVM_PARAM_COUNT ) {
        return 0u;
    }
    return s_cache[ id ];
}

void NvParams_Set( uint8_t id, uint32_t val )
{
    if ( id >= NVM_PARAM_COUNT ) {
        return;
    }
    s_cache[ id ] = val;
}

int NvParams_Save( void )
{
    Nvm_Block_t block;
    FLASH_EraseInitTypeDef erase;
    uint32_t pageError = 0u;
    HAL_StatusTypeDef hal;
    uint32_t primask;

    block.magic   = NVM_MAGIC;
    block.version = NVM_VERSION;
    memcpy( block.params, s_cache, sizeof( s_cache ) );
    block.crc     = nv_crc32( (const uint8_t *)block.params,
                              sizeof( block.params ) );

    primask = __get_PRIMASK();
    __disable_irq();

    if ( HAL_FLASH_Unlock() != HAL_OK ) {
        __set_PRIMASK( primask );
        return NVM_ERR_FLASH;
    }

    /* Erase the single parameter page. */
    erase.TypeErase   = FLASH_TYPEERASE_PAGEERASE;
    erase.PageAddress = NVM_BASE;
    erase.NbPages     = 1u;
    hal = HAL_FLASH_Erase( &erase, &pageError );
    if ( hal != HAL_OK ) {
        (void)HAL_FLASH_Lock();
        __set_PRIMASK( primask );
        return NVM_ERR_FLASH;
    }

    /* Program the whole 128-byte page. Buffer must be page-sized/aligned. */
    hal = HAL_FLASH_Program( FLASH_TYPEPROGRAM_PAGE, NVM_BASE,
                             (uint32_t *)&block );
    if ( hal != HAL_OK ) {
        (void)HAL_FLASH_Lock();
        __set_PRIMASK( primask );
        return NVM_ERR_FLASH;
    }

    (void)HAL_FLASH_Lock();
    __set_PRIMASK( primask );
    return NVM_OK;
}

void NvParams_RestoreDefaults( void )
{
    nv_load_defaults();
}