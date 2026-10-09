/**
 * @file nv_params.c
 * @brief Non-volatile parameter storage over the internal FLASH.
 *
 * Implements a single-block parameter store in the last FLASH sector
 * (page 0x0800F000). The block is one 128-byte flash page, packed by a union:
 *
 *     [ magic 4B ][ version 4B ][ crc32 4B ][ params[COUNT] ][ auto pad ]
 *
 * The pad is derived at compile time (union with a 128-byte page array), so
 * adding a parameter never needs manual byte bookkeeping.
 *
 * On power-up NvParams_Init() copies the stored block into the RAM cache after
 * validating magic/version/crc; on any mismatch it fills the cache with the
 * firmware defaults (TEC-specific, see s_defaults below - no tunable macros
 * remain in the controller code). NvParams_Save() erases the page and programms
 * the whole block again (single-block direct write, ~10k cycle endurance).
 *
 * All flash access is done directly on the FLASH registers (unlock with the
 * key sequence, page erase, page program) because this project is a pure LL
 * build and does not link the HAL flash driver. The program/erase timing
 * sequence is selected at runtime from the HSI frequency grade (ICSCR.HSI_FS),
 * mirroring the HAL __HAL_FLASH_TIMMING_SEQUENCE_CONFIG logic - it depends on
 * the HSI grade, not on SYSCLK. IRQs are disabled around the flash access so
 * the saved block is not torn by an interrupt.
 */
#include "nv_params.h"
#include "py32f0xx_ll_flash.h"
#include "py32f0xx.h"
#include "main.h"

#include <string.h>   /* memcpy */

/*============================================================================*/
/* Config                                                                      */
/*============================================================================*/
#define NVM_BASE        (0x0800F000UL)  /**< last flash sector, page-aligned  */
#define NVM_MAGIC       (0x4E564432u)   /**< 'NVD2' - non-volatile data v2     */
#define NVM_VERSION     (3u)

#define NVM_OK          (0)
#define NVM_ERR_PARAM   (-1)
#define NVM_ERR_CRC     (-2)
#define NVM_ERR_FLASH   (-3)

/* Timeout for flash busy-poll, in arbitrary iterations (~1 s of margin). */
#define NVM_FLASH_TIMEOUT  ( 1000000u )

/* Program/erase timing rows: each entry is the base address of the device
 * flash-timing parameter table in the information block (row 0 = HSI grade 0
 * (4M), 1 = 8M, 2 = 16M, 3 = 22.12M, 4 = 24M). The 5 timing values are read
 * from that address with *(uint32_t*)(base+n), exactly like the HAL
 * __HAL_FLASH_TIMMING_SEQUENCE_CONFIG macro. */
static const uint32_t s_flash_timing[ 5 ] = {
    0x1FFF0F1C,   /* 4M    */
    0x1FFF0F30,   /* 8M    */
    0x1FFF0F44,   /* 16M   */
    0x1FFF0F58,   /* 22.12M */
    0x1FFF0F6C,   /* 24M   */
};

/* Q16 fixed-point encoding for the float (PID gain) parameters. */
#define NVM_Q16( x )    ( (uint32_t)( (x) * NVM_Q16_SCALE ) )

/*============================================================================*/
/* On-flash block layout (packed into exactly one 128-byte flash page). The   */
/* union makes the reserved pad follow NVM_PARAM_COUNT automatically, so      */
/* adding a parameter never needs manual byte bookkeeping.                    */
/*============================================================================*/
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc;                       /**< CRC-32 over params[]             */
    uint32_t params[ NVM_PARAM_COUNT ];
} Nvm_Block_Data_t;

typedef union {
    Nvm_Block_Data_t  data;             /**< magic+version+crc+params          */
    uint8_t           page[ 128u ];     /**< whole flash page image            */
} Nvm_Block_t;
_Static_assert( sizeof( Nvm_Block_Data_t ) <= 128, "Nvm_Block_Data_t must fit one flash page" );
_Static_assert( sizeof( Nvm_Block_t ) == 128, "Nvm_Block_t must be one flash page" );

/*============================================================================*/
/* RAM cache + defaults                                                        */
/*============================================================================*/
static uint32_t s_cache[ NVM_PARAM_COUNT ];

/* Production defaults for every TEC slot (index order = NvParamTecId_t).
 * These match the previous tunable macros in tec_control.h/adc.h. The unit
 * is stored as-is, with float gains in Q16; scale back with
 * (float)value / NVM_Q16_SCALE. */
static const uint32_t s_defaults[ NVM_PARAM_COUNT ] = {
    [NV_TEC_PID_KP_Q16]                = NVM_Q16( 2.0f ),
    [NV_TEC_PID_KI_Q16]                = NVM_Q16( 0.1f ),
    [NV_TEC_PID_KD_Q16]                = NVM_Q16( 0.5f ),
    [NV_TEC_TARGET_BASE_MILLIC]        = 45000u,
    [NV_TEC_HOT_OVERTEMP_MILLIC]       = 60000u,
    [NV_TEC_HOT_DANGER_MILLIC]         = 70000u,
    [NV_TEC_CURRENT_FEEDFORWARD_MA]    = 2000u,
    [NV_TEC_CURRENT_DANGER_MA]         = 3000u,
    [NV_TEC_FAN_MIN_RPM]               = 500u,
    [NV_TEC_CONTROL_PERIOD_MS]         = 1000u,
    [NV_TEC_DAC_VREF_MV]               = 3300u,
    [NV_TEC_DAC_MAX_CTRL_MV]           = 1200u,
    [NV_TEC_FAN_TEMP_BASE_MILLIC]      = 40000u,
    [NV_TEC_FAN_TEMP_FULL_MILLIC]      = 60000u,
    [NV_TEC_FAN_DUTY_BASE]             = 30u,
    [NV_TEC_CURRENT_SHUNT_UOHM]        = 20000u,
    [NV_TEC_CURRENT_AMP_GAIN]          = 50u,
    [NV_TEC_CURRENT_DIVIDER_NUM]       = 1u,
    [NV_TEC_CURRENT_DIVIDER_DEN]       = 2u,
    [NV_TEC_CURRENT_ADC_VREF_MV]       = 1800u,
    [NV_TEC_TEMP_VALID_MIN_MILLIC]     = (uint32_t)( -20000 ),
    [NV_TEC_TEMP_VALID_MAX_MILLIC]     = 120000u,
    [NV_TEC_CURRENT_VALID_MAX_MA]      = 5000u,
    [NV_TEC_FAN_FAULT_MIN_DUTY]        = 30u,
    [NV_TEC_ENV_COMP_TEMP_MILLIC]      = 35000u,
    [NV_TEC_SOC_TIGHTEN_TEMP_MILLIC]   = 70000u,
    [NV_TEC_SOC_TIGHTEN_DELTA_MILLIC]  = 5000u,
    [NV_TEC_COLD_SOURCE]               = 0u,   /* 0 = TEC_COLD ADC (PA4), 1 = SDM845 */
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

/* Mirror HAL __HAL_FLASH_TIMMING_SEQUENCE_CONFIG: pick the timing table from
 * the current HSI frequency grade (ICSCR.HSI_FS) and load the 5 timing values
 * from the information block (s_flash_timing[grade] is the table base address). */
static void nv_flash_timing_config( void )
{
    uint32_t grade = ( RCC->ICSCR & RCC_ICSCR_HSI_FS ) >> RCC_ICSCR_HSI_FS_Pos;
    uint32_t base;

    if ( grade >= 5u ) {
        grade = 1u;    /* unknown grade: fall back to the 8 MHz row (default) */
    }
    base = s_flash_timing[ grade ];

    FLASH->TS0   = (*(const uint32_t *)( base + 0x00u )) & 0xFFu;
    FLASH->TS1   = ((*(const uint32_t *)( base + 0x00u )) >> 16) & 0x1FFu;
    FLASH->TS3   = ((*(const uint32_t *)( base + 0x00u )) >> 8)  & 0xFFu;
    FLASH->TS2P  = (*(const uint32_t *)( base + 0x04u )) & 0xFFu;
    FLASH->TPS3  = ((*(const uint32_t *)( base + 0x04u )) >> 16) & 0x7FFu;
    FLASH->PERTPE   = (*(const uint32_t *)( base + 0x08u )) & 0x1FFFFu;
    FLASH->SMERTPE  = (*(const uint32_t *)( base + 0x0Cu )) & 0x1FFFFu;
    FLASH->PRGTPE   = (*(const uint32_t *)( base + 0x10u )) & 0xFFFFu;
    FLASH->PRETPE   = ((*(const uint32_t *)( base + 0x10u )) >> 16) & 0x3FFFu;
}

/*============================================================================*/
/* Public API                                                                  */
/*============================================================================*/
void NvParams_Init( void )
{
    const Nvm_Block_t *stored = (const Nvm_Block_t *)NVM_BASE;

    if ( ( stored->data.magic == NVM_MAGIC ) &&
         ( stored->data.version == NVM_VERSION ) &&
         ( stored->data.crc == nv_crc32( (const uint8_t *)stored->data.params,
                                        sizeof( stored->data.params ) ) ) ) {
        memcpy( s_cache, stored->data.params, sizeof( s_cache ) );
    } else {
        nv_load_defaults();
        /* First boot / corrupted block: persist the defaults immediately so
         * the store is valid (and the defaults are no longer "unwritten") on
         * every later power-up. */
        (void)NvParams_Save();
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
    Nvm_Block_t block = { 0 };          /* zero the whole page image: the union
                                           pad bytes must be deterministic */
    uint32_t primask;
    uint32_t timeout;

    block.data.magic   = NVM_MAGIC;
    block.data.version = NVM_VERSION;
    memcpy( block.data.params, s_cache, sizeof( s_cache ) );
    block.data.crc     = nv_crc32( (const uint8_t *)block.data.params,
                                   sizeof( block.data.params ) );

    primask = __get_PRIMASK();
    __disable_irq();

    /* Unlock the FLASH interface (LL official sequence). */
    LL_FLASH_Unlock( FLASH );
    if ( ( FLASH->CR & FLASH_CR_LOCK ) != 0u ) {
        __set_PRIMASK( primask );
        return NVM_ERR_FLASH;
    }

    /* Program/erase timing from the HSI frequency grade (see
     * nv_flash_timing_config; selected at runtime, not tied to SYSCLK). */
    nv_flash_timing_config();

    /*--- page erase (official LL sequence) ----------------------------------*/
    while ( LL_FLASH_IsActiveFlag_BUSY( FLASH ) != 0u ) {
        /* wait Busy = 0 */
    }
    FLASH->SR = ( FLASH_SR_EOP | FLASH_SR_WRPERR );   /* clear status        */
    LL_FLASH_EnablePageErase( FLASH );
    LL_FLASH_SetEraseAddress( FLASH, NVM_BASE );      /* start the erase      */
    timeout = NVM_FLASH_TIMEOUT;
    while ( LL_FLASH_IsActiveFlag_BUSY( FLASH ) != 0u ) {
        if ( timeout-- == 0u ) {
            LL_FLASH_DisablePageErase( FLASH );
            __set_PRIMASK( primask );
            return NVM_ERR_FLASH;
        }
    }
    if ( LL_FLASH_IsActiveFlag_WRPERR( FLASH ) != 0u ) {
        LL_FLASH_DisablePageErase( FLASH );
        __set_PRIMASK( primask );
        return NVM_ERR_FLASH;
    }
    LL_FLASH_DisablePageErase( FLASH );

    /*--- page program (official LL sequence) --------------------------------*/
    while ( LL_FLASH_IsActiveFlag_BUSY( FLASH ) != 0u ) {
        /* wait Busy = 0 */
    }
    FLASH->SR = ( FLASH_SR_EOP | FLASH_SR_WRPERR );
    LL_FLASH_EnablePageProgram( FLASH );
    LL_FLASH_PageProgram( FLASH, NVM_BASE, (uint32_t *)&block );
    timeout = NVM_FLASH_TIMEOUT;
    while ( LL_FLASH_IsActiveFlag_BUSY( FLASH ) != 0u ) {
        if ( timeout-- == 0u ) {
            LL_FLASH_DisablePageProgram( FLASH );
            __set_PRIMASK( primask );
            return NVM_ERR_FLASH;
        }
    }
    if ( LL_FLASH_IsActiveFlag_WRPERR( FLASH ) != 0u ) {
        LL_FLASH_DisablePageProgram( FLASH );
        __set_PRIMASK( primask );
        return NVM_ERR_FLASH;
    }
    LL_FLASH_DisablePageProgram( FLASH );

    /* Re-lock the flash interface. */
    LL_FLASH_Lock( FLASH );
    __set_PRIMASK( primask );

    /*--- read-back verification: catch silent erase/program failure --------*/
    {
        const Nvm_Block_t *written = (const Nvm_Block_t *)NVM_BASE;
        if ( ( written->data.magic != block.data.magic ) ||
             ( written->data.version != block.data.version ) ||
             ( written->data.crc  != block.data.crc  ) ||
             ( memcmp( written->data.params, block.data.params,
                       sizeof( block.data.params ) ) != 0 ) ) {
            return NVM_ERR_FLASH;
        }
    }
    return NVM_OK;
}

void NvParams_RestoreDefaults( void )
{
    nv_load_defaults();
}