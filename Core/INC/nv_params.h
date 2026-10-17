/**
 * @file nv_params.h
 * @brief Non-volatile parameter storage over the internal FLASH.
 *
 * A fixed-size table of uint32 parameters is kept in a RAM cache and
 * persisted to the last FLASH sector on demand. The store is dual-slot:
 * two identical 256-byte blocks (each two 128-byte flash pages) at
 * 0x0800F000 (slot A) and 0x0800F100 (slot B). Layout per slot:
 * [ magic ][ version ][ seq ][ crc32 ][ params[] ]; the block is packed
 * by a union in nv_params.c, so the reserved pad is derived by the
 * compiler, not hand-written.
 *
 *   Base: 0x0800F000 (last sector of the 64 KB device, page-aligned);
 *   the A/B pair uses the first 512 B of the reserved 4 KB sector.
 *
 * The layout is TEC-specific: every tunable controller/sensor parameter has
 * a named slot (NvParamTecId_t). Float gains are stored as Q16 fixed point
 * (value x 65536) so a plain uint32 slot holds them exactly; convert back
 * with (float)raw / 65536.0f. There are no tunable macros in the firmware
 * anymore: tec_control/adc read their configuration from here at power-up.
 *
 * Adding a parameter = adding one enum line in NvParamTecId_t (before the
 * NV_PARAM_COUNT_MARKER sentinel) and one default in s_defaults. The count,
 * the on-flash block size and the I2C window (which derives from
 * NVM_PARAM_COUNT) all follow automatically. Bump NVM_VERSION when the
 * on-flash layout changes (the sentinel/union handles the size, not the
 * version).
 *
 * Write strategy is alternating dual-slot: NvParams_Save() erases and
 * programs the slot that does NOT hold the newest good block, verifies the
 * programmed image by read-back, and only then flips the active slot
 * (Init() also picks by seq, so a reset before the flip is harmless). A
 * power failure mid-Save therefore always leaves one CRC-valid block -
 * either the previous or the new parameters - instead of a torn store.
 * Endurance is ~10k erase cycles per page on PY32, doubled by the A/B
 * alternation; fine for parameters that are written rarely.
 *
 * Usage:
 *   NvParams_Init();                    // once, at power-up (loads or defaults)
 *   ... NvParams_Get(NV_TEC_PID_KP_Q16) / NvParams_Set(id, v) ...
 *   NvParams_Save();                    // persist the RAM cache to FLASH
 *
 * The I2C slave exposes every slot as a 4-byte big-endian window at
 * 0x40..0xCF (tres_i2c_slave.h), plus SAVE_PARAMS / DEFAULT_PARAMS / RELOAD_PARAMS commands.
 */
#ifndef __NV_PARAMS_H__
#define __NV_PARAMS_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many uint32 parameter slots are persisted. Derived from the enum
 * sentinel NV_PARAM_COUNT_MARKER: adding a slot to NvParamTecId_t automatically
 * grows the count (and the on-flash block) - no manual bookkeeping. */
#define NVM_PARAM_COUNT      ( (uint8_t)NV_PARAM_COUNT_MARKER )

/* Q16 fixed-point scale for float parameters stored as uint32. */
#define NVM_Q16_SCALE        (65536.0f)

/**
 * @brief TEC parameter slots (also the I2C window slot index: slot n lives at
 *        register 0x44 + 4*n, big-endian). Do not reorder once deployed; bump
 *        NVM_VERSION instead on a layout change.
 *
 * SIGN CONVENTION: every slot is transferred as a 4-byte UNSIGNED big-endian
 * word, but the milli-degree temperature slots are SIGNED int32 inside the
 * firmware and are carried as two's complement on the wire (the firmware
 * casts the uint32 back to int32). A host must reinterpret those slots as
 * signed: reading TEMP_VALID_MIN's default 0xFFFFB1E0 as 4294947296 instead
 * of -20000 is the classic mistake. The affected slots are TARGET_BASE,
 * TEMP_VALID_MIN and TEMP_VALID_MAX - the latter two hold negative
 * temperatures by definition, TARGET_BASE is signed for symmetry.
 * TARGET_OFFSET is deliberately NOT one of them: it is an unsigned magnitude
 * and the firmware applies the (downward) direction itself.
 * test/tres_i2c_probe.py keeps the matching NV_SIGNED_SLOTS list.
 */
typedef enum {
    NV_TEC_PID_KP_Q16 = 0u,            /**< PID P gain (Q16): %out per m°C      */
    NV_TEC_PID_KI_Q16,                 /**< PID I gain (Q16): %out per m°C per
                                            SECOND (scaled by loop period_s
                                            inside TecPid_Init)                  */
    NV_TEC_PID_KD_Q16,                 /**< PID D gain (Q16): %out per m°C per
                                            SECOND (scaled by loop period_s
                                            inside TecPid_Init)                  */
    NV_TEC_TARGET_BASE_MILLIC,         /**< base TEC target, milli-degC         */
    NV_TEC_HOT_OVERTEMP_MILLIC,        /**< hot > this: fan full speed (m°C)    */
    NV_TEC_HOT_DANGER_MILLIC,          /**< hot > this: TEC off + fan 100 (m°C) */
    NV_TEC_CURRENT_FEEDFORWARD_MA,     /**< DEPRECATED/RESERVED: the current-
                                            derived feedforward was removed
                                            (positive feedback, loop gain >1 in
                                            the FF band - duty rises, current
                                            rises, term adds more duty). Slot
                                            kept so later slots keep their
                                            register addresses; no firmware
                                            consumer reads it.                  */
    NV_TEC_CURRENT_DANGER_MA,          /**< current above this: alarm +FF50 (mA)*/
    NV_TEC_FAN_MIN_RPM,                /**< fan fault threshold (RPM)           */
    NV_TEC_CONTROL_PERIOD_MS,          /**< control/QuarkTS period (ms)         */
    NV_TEC_DAC_MAX_CTRL_MV,            /**< FB-injection window top = TEC OFF end
                                            (VOUT pins ≈0 V); R3=20k → ≈2500 mV */
    NV_TEC_FAN_TEMP_BASE_MILLIC,       /**< fan starts ramping at this hot (m°C)*/
    NV_TEC_FAN_TEMP_FULL_MILLIC,       /**< fan full speed at this hot (m°C)    */
    NV_TEC_FAN_DUTY_BASE,              /**< base airflow while TEC active (%)   */
    NV_TEC_CURRENT_SHUNT_UOHM,         /**< INA181A2 shunt resistance (µOhm)    */
    NV_TEC_CURRENT_AMP_GAIN,           /**< INA181A2 gain (50 V/V)              */
    NV_TEC_CURRENT_DIVIDER_NUM,        /**< sense divider numerator             */
    NV_TEC_CURRENT_DIVIDER_DEN,        /**< sense divider denominator           */
    NV_TEC_CURRENT_ADC_VREF_MV,        /**< ADC reference for the sense chain   */
    NV_TEC_TEMP_VALID_MIN_MILLIC,      /**< lowest plausible temperature (m°C)  */
    NV_TEC_TEMP_VALID_MAX_MILLIC,      /**< highest plausible temperature (m°C) */
    NV_TEC_CURRENT_VALID_MAX_MA,       /**< highest plausible TEC current (mA)  */
    NV_TEC_FAN_FAULT_MIN_DUTY,         /**< fan fault needs duty above this (%) */
    NV_TEC_ENV_COMP_TEMP_MILLIC,       /**< env above this raises the target    */
    NV_TEC_SOC_TIGHTEN_TEMP_MILLIC,    /**< SDM above this tightens the target  */
    NV_TEC_SOC_TIGHTEN_DELTA_MILLIC,   /**< by how much it tightens (m°C)       */
    NV_TEC_COLD_SOURCE,                /**< cold-side temperature source select:
                                            0 = TEC_COLD ADC (PA4), 1 = SDM845   */
    NV_TEC_DAC_STARTUP_PCT,            /**< first-enable anchor as % of full
                                            cooling (0..100). Under the
                                            calibrated window duty% == TEC
                                            voltage % of 6 V, so default 10 (%)
                                            starts at ≈0.6 V and the ramp climbs.
                                            See R3 / endpoint calibration in
                                            WORKLOG "FB 注入标定".                */
    NV_TEC_DAC_MIN_CTRL_MV,             /**< FB-injection window bottom = FULL-
                                            cooling end: DAC = FB = VFBTH
                                            (0.76 V), zero injection, VOUT at
                                            the 6 V natural set point. Never
                                            command below - lower DAC sinks
                                            current out of FB and overdrives
                                            VOUT past 6 V                        */
    NV_TEC_RAMP_STEP_PERCENT,          /**< max duty % change PER CONTROL CYCLE
                                            (deliberately per-cycle, not per-
                                            second: changing the control period
                                            changes the physical ramp rate, so
                                            retune this slot when the period
                                            changes)                           */
    NV_TEC_DEWPOINT_MARGIN_MILLIC,     /**< target floor: dew point + this     */
    NV_TEC_FF_GAIN_Q16,                /**< COMMON-MODE feedforward: % duty per
                                            °C of (env - target) below knee    */
    NV_TEC_HOT_FB_GAIN_Q16,            /**< hot-side foldback gain: cold-target
                                            lift in m°C per m°C of hot excess
                                            over the overtemp knee (Q16)       */
    NV_TEC_HOT_FB_MAX_MILLIC,          /**< hot-side foldback: max cold-target
                                            lift (m°C)                         */
    NV_TEC_FF_ENABLE,                  /**< feedforward master switch: 0 = PID
                                            only, nonzero = common-mode
                                            feedforward enabled (the only FF
                                            term; the current-derived term was
                                            removed as positive feedback).
                                            Default OFF: an additive FF sits
                                            OUTSIDE the PID's [0,100] domain, so
                                            an overestimate acts as a duty floor
                                            the loop can never go below (PID
                                            pinned at out_min cannot subtract)
                                            and the cold side settles away from
                                            the target.                         */
    NV_TEC_EN_OFF_PCT,                 /**< anti-hiccup deadband: duty below this
                                            drops TEC_VEN (the DCDC EN) instead
                                            of driving VOUT into the TPS565201
                                            UVP-hiccup region (inductor whine).
                                            The SAME value is also the re-enable
                                            threshold (no fixed hysteresis).
                                            Default 8 % ~= 0.5 V on the 6 V TEC;
                                            bench-calibrate the boundary.        */
    NV_TEC_TARGET_OFFSET_MILLIC,       /**< fixed cold-target offset, milli-degC.
                                            UNSIGNED magnitude, default 0, valid
                                            0..20000 (TEC_TARGET_OFFSET_MAX_MILLIC).
                                            The NTC sits on the cold plate, not on
                                            the die, so the die runs hotter than
                                            this sensor reads. The firmware
                                            compensates by LOWERING the cold-side
                                            target by exactly this amount
                                            (target_mc -= offset): a value of
                                            5000 aims the measured target 5 C
                                            BELOW where it would otherwise sit.
                                            Direction is the firmware's job -
                                            enter a magnitude only; a negative
                                            value is out of range and falls back
                                            to 0.
                                            Applied AFTER the dew-point floor, so
                                            it may also take the target below that
                                            floor; the 0..20000 range bounds that
                                            excursion to at most 20 C.
                                            The hot-side foldback lift is added
                                            AFTER it, so this slot can never
                                            cancel the heatsink derating.          */
    NV_PARAM_COUNT_MARKER,             /**< sentinel: MUST stay last. NVM_PARAM_COUNT
                                            is derived from it, so adding a slot
                                            is just adding a line above.         */
} NvParamTecId_t;

/**
 * @brief Load the parameter cache from FLASH at power-up. Validates both
 *        slots (magic, version, CRC) and takes the newest good one (wrap-aware
 *        seq, preferring protected v5 over legacy v4); if neither slot holds a valid block, fills the cache with
 *        the firmware defaults and persists them to FLASH immediately
 *        (so the store is valid on every later power-up).
 */
void  NvParams_Init(void);
/* Reload saved FLASH into RAM; 0 on success, cache unchanged on failure. */
int NvParams_Reload(void);

/**
 * @brief Read a parameter from the RAM cache.
 * @param id  Parameter slot (0..NVM_PARAM_COUNT-1).
 * @return The cached value, or 0 if `id` is out of range.
 */
uint32_t NvParams_Get(uint8_t id);

/**
 * @brief Read the factory default of a parameter (independent of the RAM
 *        cache and of the FLASH store, so it stays trustworthy even when
 *        the cached value has been judged corrupt). Use this - not a
 *        hand-copied constant - as the fallback in sanity checks.
 * @param id  Parameter slot (0..NVM_PARAM_COUNT-1).
 * @return The s_defaults value, or 0 if `id` is out of range.
 */
uint32_t NvParams_GetDefault(uint8_t id);

/**
 * @brief Write a parameter to the RAM cache (RAM only; call #NvParams_Save to
 *        persist to FLASH).
 * @param id  Parameter slot (0..NVM_PARAM_COUNT-1).
 * @param val Value to store.
 */
void NvParams_Set(uint8_t id, uint32_t val);

/**
 * @brief Monotonic generation counter for the RAM cache.
 *
 * Bumped by every path that changes the cache: #NvParams_Set, a successful
 * #NvParams_Reload, #NvParams_RestoreDefaults and the power-up default fill.
 * A consumer that snapshots parameter values at init (for example the TEC
 * control task, which copies the whole table once) compares this against its
 * own copy to detect that the values it is using are stale, then reloads.
 *
 * Only equality matters - treat it as "changed / unchanged", never as an
 * ordering or a timestamp. Safe to call from task context even though
 * #NvParams_Set may run in an ISR: a lost update at worst delays a consumer's
 * reload by one of its own cycles.
 *
 * @return The current cache generation.
 */
uint32_t NvParams_GetCacheGeneration(void);

/**
 * @brief Persist the RAM cache to FLASH: erase and program the slot that
 *        does NOT hold the newest good block (with recomputed CRC and a
 *        monotonically increasing seq), verify the programmed image by
 *        read-back, then flip the active slot. A power failure mid-Save
 *        leaves the previous slot intact. Blocking, and disables IRQs
 *        around the flash access.
 * @return 0 on success, or a negative error code on failure.
 */
int   NvParams_Save(void);

/**
 * @brief Restore the RAM cache to the firmware defaults. Does not write to
 *        FLASH; call #NvParams_Save to persist them.
 */
void  NvParams_RestoreDefaults(void);

#ifdef __cplusplus
}
#endif

#endif /* __NV_PARAMS_H__ */
