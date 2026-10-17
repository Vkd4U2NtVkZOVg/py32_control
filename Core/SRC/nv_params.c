/**
 * @file nv_params.c
 * @brief Non-volatile parameter storage over the internal FLASH.
 *
 * Two page-aligned A/B records occupy the reserved last FLASH sector.
 * Each v5 record stores [magic][version][seq][crc][params][page padding].
 * CRC covers seq + params. Save writes and verifies the inactive record;
 * a torn write preserves the previous complete record. Sequence ordering
 * handles uint32 wrap. Valid legacy v4 records are migrated copy-on-write
 * at startup without resetting calibration; v5 records take precedence.
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
#define NVM_MAGIC       (0x4E564433u)   /**< 'NVD3'                            */
#define NVM_VERSION     (7u)            /**< v7: TARGET_OFFSET meaning changed:
                                             unsigned 0..20000 magnitude that
                                             LOWERS the target (v6 stored a
                                             signed +/-100 C correction)        */
#define NVM_PAGE_BYTES  (128u)          /**< PY32F030 flash page size          */
#define NVM_SLOTS       (2u)            /**< A/B slots; Save flips between them*/

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

/* Slot base addresses: slot i starts at NVM_BASE + i * NVM_BLOCK_SIZE. The
 * A/B pair occupies 2 * NVM_BLOCK_SIZE bytes inside the reserved 4 KB NVM
 * sector. */
#define NVM_SLOT_ADDR( i )  ( NVM_BASE + ( (uint32_t)( i ) * NVM_BLOCK_SIZE ) )

/*============================================================================*/
/* On-flash block layout (NVM_PAGES consecutive 128-byte flash pages per      */
/* slot, derived from the block size below - adding a parameter grows the     */
/* page count automatically). seq grows on every Save so Init() can tell      */
/* which slot holds the newest good block.                                    */
/*============================================================================*/
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;                       /**< write generation (monotonic)      */
    uint32_t crc;                       /**< CRC-32 over seq + params[]             */
    uint32_t params[ NVM_PARAM_COUNT ];
} Nvm_Block_Data_t;

/* Pages per slot, derived from the actual block size: a 148-byte block
 * takes 2 pages, a block <= 128 bytes would take 1, etc. No manual
 * bookkeeping when the parameter table grows or shrinks. */
#define NVM_PAGES       ( ( sizeof( Nvm_Block_Data_t ) + NVM_PAGE_BYTES - 1u ) / NVM_PAGE_BYTES )
#define NVM_BLOCK_SIZE  ( NVM_PAGE_BYTES * NVM_PAGES )

typedef union {
    Nvm_Block_Data_t  data;             /**< magic+version+seq+crc+params      */
    uint8_t           page[ NVM_BLOCK_SIZE ];
} Nvm_Block_t;
_Static_assert( sizeof( Nvm_Block_Data_t ) <= NVM_BLOCK_SIZE,
                "Nvm_Block_Data_t must fit the NVM flash pages" );
_Static_assert( sizeof( Nvm_Block_t ) == NVM_BLOCK_SIZE,
                "Nvm_Block_t must equal the NVM flash pages" );
/* Dual-slot budget: both slots together must stay inside the reserved 4 KB
 * NVM sector (page-aligned erase always touches whole pages). */
_Static_assert( ( NVM_SLOTS * NVM_BLOCK_SIZE ) <= 4096u,
                "dual-slot NVM store exceeds the reserved 4 KB sector" );

/*============================================================================*/
/* RAM cache + defaults                                                        */
/*============================================================================*/
static uint32_t s_cache[ NVM_PARAM_COUNT ];
static uint32_t s_active_slot;      /**< slot holding the newest good block  */
static uint32_t s_active_seq;       /**< its seq (next Save writes seq+1)    */
static uint8_t  s_active_valid;     /**< 0 until a good block was seen/written*/

/* Cache generation: bumped by EVERY path that changes s_cache, so consumers
 * that snapshot parameter values (tec_control.c's Tec_Params_t) can tell that
 * their copy is stale and reload it. Never decremented; comparisons are
 * "different from my copy", not ordering, so a single wrap after 2^32 changes
 * is harmless.
 *
 * volatile because NvParams_Set() is called from the I2C slave ISR while the
 * readers run in task context. It is a pure change indicator, so a lost update
 * under a race at worst delays a consumer's reload by one of its own cycles -
 * it can never make a consumer miss a change permanently. */
static volatile uint32_t s_cache_gen;

/* Single funnel for every cache mutation: keeps the generation count honest
 * instead of requiring each writer to remember to bump it. */
static void nv_cache_bump( void )
{
    s_cache_gen++;
}

/* Production defaults for every TEC slot (index order = NvParamTecId_t).
 * These match the previous tunable macros in tec_control.h/adc.h. The unit
 * is stored as-is, with float gains in Q16; scale back with
 * (float)value / NVM_Q16_SCALE. */
static const uint32_t s_defaults[ NVM_PARAM_COUNT ] = {
    [NV_TEC_PID_KP_Q16]                = NVM_Q16( 0.01f ),
    [NV_TEC_PID_KI_Q16]                = NVM_Q16( 0.01f ),
    [NV_TEC_PID_KD_Q16]                = NVM_Q16( 0.25f ),
    [NV_TEC_TARGET_BASE_MILLIC]        = 45000u,
    [NV_TEC_HOT_OVERTEMP_MILLIC]       = 80000u,
    [NV_TEC_HOT_DANGER_MILLIC]         = 90000u,
    [NV_TEC_CURRENT_FEEDFORWARD_MA]    = 2000u,/* RESERVED, no consumer: the current-
                                                  derived FF was removed (positive
                                                  feedback). Slot kept so later slots
                                                  keep their I2C register addresses. */
    [NV_TEC_CURRENT_DANGER_MA]         = 3000u,
    [NV_TEC_FAN_MIN_RPM]               = 500u,
    [NV_TEC_CONTROL_PERIOD_MS]         = 500u,
    [NV_TEC_DAC_MAX_CTRL_MV]           = 1600u,/* FB-injection window top = TEC OFF
                                                  end: injection cancels the R2
                                                  current and VOUT pins at ≈0 V.
                                                  R3=20k theory puts the pin at
                                                  ≈2.50 V (WORKLOG R3 math) - the old
                                                  1200 mV only throttled to ≈4.4 V
                                                  and never turned the TEC off.
                                                  Bench-calibrate the exact point.  */
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
    [NV_TEC_ENV_COMP_TEMP_MILLIC]      = 50000u,
    [NV_TEC_SOC_TIGHTEN_TEMP_MILLIC]   = 70000u,
    [NV_TEC_SOC_TIGHTEN_DELTA_MILLIC]  = 5000u,
    [NV_TEC_COLD_SOURCE]               = 0u,   /* 0 = TEC_COLD ADC (PA4), 1 = SDM845 */
    [NV_TEC_DAC_STARTUP_PCT]            = 10u,  /* first-enable anchor as % of full
                                                  cooling. Under the calibrated
                                                  window [760, 2500] duty% == TEC
                                                  voltage % of the 6 V rating, so
                                                  10 % starts at ≈0.6 V and the
                                                  per-cycle ramp (RAMP_STEP_PERCENT)
                                                  climbs from there - true low-
                                                  voltage soft start, no half-supply
                                                  slam. Change over NVRAM/probe.   */
    [NV_TEC_DAC_MIN_CTRL_MV]           = 760u, /* FB-injection window bottom =
                                                  FULL-COOLING end: DAC = FB =
                                                  VFBTH (0.76 V) → zero injection
                                                  current → VOUT = divider natural
                                                  set point (6 V). Never command
                                                  below: a lower DAC SINKS current
                                                  out of the FB node and OVERDRIVES
                                                  VOUT past 6 V (TEC overvoltage).
                                                  Theory 760 mV; bench-calibrate.  */
    [NV_TEC_RAMP_STEP_PERCENT]         = 5u,   /* 5 % duty per 1 s cycle            */
    [NV_TEC_DEWPOINT_MARGIN_MILLIC]    = 2000u,/* 2 °C above dew point             */
    [NV_TEC_FF_GAIN_Q16]               = NVM_Q16( 1.0f ),  /* 1 % duty per °C      */
    [NV_TEC_HOT_FB_GAIN_Q16]           = NVM_Q16( 2.0f ),  /* cold target +2 °C
                                                              per 1 °C of hot
                                                              excess over the
                                                              overtemp knee     */
    [NV_TEC_HOT_FB_MAX_MILLIC]         = 30000u,/* foldback lift cap: +30 °C        */
    [NV_TEC_FF_ENABLE]                 = 0u,   /* feedforward master switch: default
                                                  OFF (bench: additive FF acted as a
                                                  duty floor the loop could not go
                                                  below, cold side never settled at
                                                  target). 0 = PID only; nonzero =
                                                  common-mode FF on (the only term -
                                                  the current-derived FF was removed
                                                  as positive feedback). Write
                                                  slot 34 (reg 0xC8) to toggle.     */
    [NV_TEC_EN_OFF_PCT]                = 8u,   /* anti-hiccup deadband: duty below
                                                  this drops TEC_VEN (DCDC EN) - a
                                                  high DAC drives VOUT into the
                                                  TPS565201 UVP-hiccup region and
                                                  the inductor whines; a disabled
                                                  converter has zero TEC current and
                                                  the cold side warms back up. The
                                                  same value is also the re-enable
                                                  threshold (no hysteresis); bench-
                                                  calibrate the boundary.     */
    [NV_TEC_TARGET_OFFSET_MILLIC]      = 0u,   /* NTC-to-CPU gap: UNSIGNED magnitude
                                                  0..20000; the firmware LOWERS the
                                                  cold target by this much, so it
                                                  may sit below the dew floor.
                                                  Default 0 = no compensation       */
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

/* Include the generation without changing the existing on-flash offsets. */
static uint32_t nv_block_crc(const Nvm_Block_Data_t *block)
{
    const uint8_t *seq = (const uint8_t *)&block->seq;
    const uint8_t *params = (const uint8_t *)block->params;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    uint8_t bit;
    /* Stream both regions without a second parameter-sized stack buffer. */
    for (i = 0u; i < sizeof(block->seq) + sizeof(block->params); ++i) {
        crc ^= (i < sizeof(block->seq)) ? seq[i] : params[i - sizeof(block->seq)];
        for (bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 1u) ? (crc >> 1u) ^ 0xEDB88320u : (crc >> 1u);
        }
    }
    return ~crc;
}

/* Raw table replacement - NO critical section of its own. Callers decide,
 * because the required protection differs: NvParams_Init() runs before the
 * I2C slave ISR exists (nothing to guard against), NvParams_Set() is already
 * a single aligned slot store, while the bulk writers
 * (NvParams_Reload/NvParams_RestoreDefaults) must disable interrupts around
 * it or the ISR can interleave a one-slot write into the copy. */
static void nv_load_defaults( void )
{
    memcpy( s_cache, s_defaults, sizeof( s_cache ) );
    nv_cache_bump();
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
int NvParams_Reload( void )
{
    static const Nvm_Block_t * const slot[ NVM_SLOTS ] = {
        (const Nvm_Block_t *)NVM_SLOT_ADDR( 0 ),
        (const Nvm_Block_t *)NVM_SLOT_ADDR( 1 ),
    };
    uint8_t valid[ NVM_SLOTS ];
    uint32_t i;

    for ( i = 0u ; i < NVM_SLOTS ; ++i ) {
        uint32_t version = slot[i]->data.version;
        valid[i] = (slot[i]->data.magic == NVM_MAGIC) &&
            (((version == NVM_VERSION) && (slot[i]->data.crc == nv_block_crc(&slot[i]->data))) ||
             ((version == 4u) && (slot[i]->data.crc == nv_crc32(
                 (const uint8_t *)slot[i]->data.params, sizeof(slot[i]->data.params)))));
    }

    /* Newest good block wins: with both slots valid wrap-aware sequence order identifies the
     * last completed Save (a Save that was interrupted mid-write leaves its
     * target slot CRC-invalid, so the previous slot still wins). */
    if ( valid[ 0 ] != 0u && valid[ 1 ] != 0u ) {
        /* A protected v5 record always wins over a legacy v4 record. */
        if (slot[0]->data.version != slot[1]->data.version) {
            s_active_slot = (slot[1]->data.version == NVM_VERSION) ? 1u : 0u;
        } else {
            uint32_t delta = slot[1]->data.seq - slot[0]->data.seq;
            s_active_slot = (delta != 0u && delta < 0x80000000u) ? 1u : 0u;
        }
    } else if ( valid[ 1 ] != 0u ) {
        s_active_slot = 1u;
    } else {
        s_active_slot = 0u;
    }
    s_active_valid = valid[ s_active_slot ];

    if ( s_active_valid != 0u ) {
        uint32_t primask = __get_PRIMASK();

        /* Replace the table inside a critical section. NvParams_Set() runs in
         * the I2C slave ISR and writes s_cache one slot at a time, so an
         * unprotected memcpy could leave a permanently torn table (old and new
         * slots mixed) for every reader to snapshot - the generation bump that
         * follows would then advertise that torn table as the current one.
         * NvParams_Save() already guards its own snapshot the same way.
         * Losing a concurrent one-slot ISR write here is CORRECT, not a bug:
         * reload means "discard the RAM table and take the FLASH image".
         *
         * The bump is inside the region as well, so readers cannot observe the
         * new generation together with the old (or a half-copied) table. */
        __disable_irq();
        s_active_seq = slot[ s_active_slot ]->data.seq;
        memcpy( s_cache, slot[ s_active_slot ]->data.params, sizeof( s_cache ) );
        nv_cache_bump();
        __set_PRIMASK( primask );
        return NVM_OK;
    }
    return NVM_ERR_CRC; /* leave the RAM table untouched on reload failure */
}

void NvParams_Init(void)
{
    if (NvParams_Reload() != NVM_OK) {
        nv_load_defaults();
        s_active_seq = 0u;
        (void)NvParams_Save();
    } else {
        const Nvm_Block_t *active = (const Nvm_Block_t *)NVM_SLOT_ADDR(s_active_slot);
        if (active->data.version == 4u) {
            /* Copy-on-write migration: keep the old calibrated record intact. */
            (void)NvParams_Save();
        }
    }
}

uint32_t NvParams_Get( uint8_t id )
{
    if ( id >= NVM_PARAM_COUNT ) {
        return 0u;
    }
    return s_cache[ id ];
}

uint32_t NvParams_GetDefault( uint8_t id )
{
    if ( id >= NVM_PARAM_COUNT ) {
        return 0u;
    }
    return s_defaults[ id ];
}

void NvParams_Set( uint8_t id, uint32_t val )
{
    if ( id >= NVM_PARAM_COUNT ) {
        return;
    }
    s_cache[ id ] = val;
    nv_cache_bump();
}

uint32_t NvParams_GetCacheGeneration( void )
{
    return s_cache_gen;
}

int NvParams_Save( void )
{
    Nvm_Block_t block = { 0 };          /* zero the whole page image: the union
                                           pad bytes must be deterministic */
    uint32_t primask;
    uint32_t timeout;
    uint32_t target_slot;
    uint32_t page;
    uint32_t slot_base;

    block.data.magic   = NVM_MAGIC;
    block.data.version = NVM_VERSION;

    /* Dual-slot power-fail strategy: always write the slot that does NOT
     * hold the newest good block. A reset mid-write only damages that
     * target slot; the previous slot stays CRC-valid, so Init() recovers
     * the old parameters. Only after the read-back verify passes does the
     * active slot flip - and even a reset before the flip is harmless,
     * because Init() picks by seq, not by the RAM flag. */
    target_slot = ( s_active_valid != 0u ) ? ( 1u - s_active_slot ) : s_active_slot;
    slot_base   = NVM_SLOT_ADDR( target_slot );

    primask = __get_PRIMASK();
    __disable_irq();

    /* Snapshot the cache inside the critical section: the I2C slave ISR
     * writes s_cache one slot at a time, so the FLASH image must capture a
     * consistent table, not one mixing several in-flight host writes.
     * (The FLASH erase/program/verify below is already IRQ-blocked: the
     * CPU fetches its ISR code from the very array being programmed.) */
    block.data.seq     = s_active_seq + 1u;
    memcpy( block.data.params, s_cache, sizeof( s_cache ) );
    block.data.crc     = nv_block_crc(&block.data);

    /* Unlock the FLASH interface (LL official sequence). */
    LL_FLASH_Unlock( FLASH );
    if ( ( FLASH->CR & FLASH_CR_LOCK ) != 0u ) {
        __set_PRIMASK( primask );
        return NVM_ERR_FLASH;
    }

    /* Program/erase timing from the HSI frequency grade (see
     * nv_flash_timing_config; selected at runtime, not tied to SYSCLK). */
    nv_flash_timing_config();

    /*--- page erase (official LL sequence), NVM_PAGES pages -----------------*/
    for ( page = 0u ; page < NVM_PAGES ; ++page ) {
        timeout = NVM_FLASH_TIMEOUT;
        while ( LL_FLASH_IsActiveFlag_BUSY( FLASH ) != 0u ) {
            if ( timeout-- == 0u ) {
                LL_FLASH_Lock( FLASH );
                __set_PRIMASK( primask );
                return NVM_ERR_FLASH;
            }
        }
        FLASH->SR = ( FLASH_SR_EOP | FLASH_SR_WRPERR );   /* clear status    */
        LL_FLASH_EnablePageErase( FLASH );
        LL_FLASH_SetEraseAddress( FLASH, slot_base + page * 128u ); /* erase */
        timeout = NVM_FLASH_TIMEOUT;
        while ( LL_FLASH_IsActiveFlag_BUSY( FLASH ) != 0u ) {
            if ( timeout-- == 0u ) {
                LL_FLASH_DisablePageErase( FLASH );
                LL_FLASH_Lock( FLASH );   /* every exit path re-locks */
                __set_PRIMASK( primask );
                return NVM_ERR_FLASH;
            }
        }
        if ( LL_FLASH_IsActiveFlag_WRPERR( FLASH ) != 0u ) {
            LL_FLASH_DisablePageErase( FLASH );
            LL_FLASH_Lock( FLASH );   /* every exit path re-locks */
            __set_PRIMASK( primask );
            return NVM_ERR_FLASH;
        }
        LL_FLASH_DisablePageErase( FLASH );
    }

    /*--- page program (official LL sequence), NVM_PAGES pages ----------------*/
    for ( page = 0u ; page < NVM_PAGES ; ++page ) {
        uint8_t *src = (uint8_t *)&block + page * 128u;

        timeout = NVM_FLASH_TIMEOUT;
        while ( LL_FLASH_IsActiveFlag_BUSY( FLASH ) != 0u ) {
            if ( timeout-- == 0u ) {
                LL_FLASH_Lock( FLASH );
                __set_PRIMASK( primask );
                return NVM_ERR_FLASH;
            }
        }
        FLASH->SR = ( FLASH_SR_EOP | FLASH_SR_WRPERR );
        LL_FLASH_EnablePageProgram( FLASH );
        LL_FLASH_PageProgram( FLASH, slot_base + page * 128u, (uint32_t *)src );
        timeout = NVM_FLASH_TIMEOUT;
        while ( LL_FLASH_IsActiveFlag_BUSY( FLASH ) != 0u ) {
            if ( timeout-- == 0u ) {
                LL_FLASH_DisablePageProgram( FLASH );
                LL_FLASH_Lock( FLASH );   /* every exit path re-locks */
                __set_PRIMASK( primask );
                return NVM_ERR_FLASH;
            }
        }
        if ( LL_FLASH_IsActiveFlag_WRPERR( FLASH ) != 0u ) {
            LL_FLASH_DisablePageProgram( FLASH );
            LL_FLASH_Lock( FLASH );   /* every exit path re-locks */
            __set_PRIMASK( primask );
            return NVM_ERR_FLASH;
        }
        LL_FLASH_DisablePageProgram( FLASH );
    }

    /* Re-lock the flash interface. */
    LL_FLASH_Lock( FLASH );
    __set_PRIMASK( primask );

    /*--- read-back verification of the slot just written ---------------------*/
    {
        const Nvm_Block_t *written = (const Nvm_Block_t *)slot_base;
        if ( ( written->data.magic != block.data.magic ) ||
             ( written->data.version != block.data.version ) ||
             ( written->data.seq != block.data.seq ) ||
             ( written->data.crc  != block.data.crc  ) ||
             ( memcmp( written->data.params, block.data.params,
                       sizeof( block.data.params ) ) != 0 ) ) {
            return NVM_ERR_FLASH;
        }
    }

    /* Verified: flip the active slot only now. */
    s_active_slot  = target_slot;
    s_active_seq   = block.data.seq;
    s_active_valid = 1u;
    return NVM_OK;
}

void NvParams_RestoreDefaults( void )
{
    uint32_t primask = __get_PRIMASK();

    /* Same torn-table hazard as NvParams_Reload(): the ISR writes s_cache slot
     * by slot, so the bulk replacement and the generation bump must be atomic
     * with respect to it. */
    __disable_irq();
    nv_load_defaults();
    __set_PRIMASK( primask );
}
