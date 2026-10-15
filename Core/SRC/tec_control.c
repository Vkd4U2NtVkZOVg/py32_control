/**
 * @file tec_control.c
 * @brief TEC control task (QuarkTS periodic task).
 *
 * Reads all temperature/current sources, controls the COLD side (the device
 * being cooled) with PID + feedforward, applies fault protection, and emits
 * the TEC current command through the MCP4725 DAC into the power-stage control
 * window. The heatsink fan is decoupled and follows the hot-side temperature.
 * Non-blocking: a plain periodic QuarkTS task (period from NVRAM, default 1 s)
 * that runs to completion and returns.
 *
 * Control structure (deliberate separation of roles):
 *   - cold side (sdm_mc) : the controlled variable - PID feedback + dew-point
 *                          protection + SoC-tighten target input
 *   - hot side (hot_mc)  : protection input only - danger shutdown, warning
 *                          flag, and fan ramping; never feeds the PID error
 *   - ambient (env_mc)   : target compensation and common-mode feedforward,
 *                          not the PID measure
 * Dew-point protection raises the target floor and holds minimum cooling
 * with temperature hysteresis. It does not guarantee condensation avoidance
 * if the minimum stable converter output still overcools the cold side.
 *
 * Every tunable parameter lives in NVRAM (nv_params.h); TecControl_Init()
 * snapshots the whole table into a local struct, so the control loop never
 * re-reads FLASH-backed storage per cycle.
 */
#include "tec_control.h"
#include "main.h"
#include "QuarkTS.h"
#include "gpio.h"
#include "fan.h"
#include "fan_task.h"
#include "aht20_task.h"
#include "adc.h"
#include "tres_i2c_slave.h"
#include "tec_pid.h"
#include "sdm845_boot.h"
#include "mcp4725.h"
#include "nv_params.h"

/*============================================================================*/
/* Fixed hysteresis between the EN cut-off threshold and the re-enable        */
/* threshold (percent of duty). Keeps the EN boundary from chattering while   */
/* the demand hovers around the anti-hiccup deadband.                          */
/*============================================================================*/
#define TEC_EN_HYST_PCT     ( 4 )
#define TEC_DEW_RELEASE_MILLIC ( 1000 )

/* Absolute envelope for every milli-degree parameter: the NTC sensors span
 * -40..125 C, so anything outside [-40, 200] C is corrupt (host miswrite,
 * FLASH bit flip). Falling back per parameter also makes every downstream
 * int32 subtraction/multiplication safe: all inputs then span at most
 * 240 K, so the fan-ramp and foldback products stay far inside 32 bits. */
#define TEC_TEMP_ENVELOPE_MIN_MILLIC   ( -40000 )
#define TEC_TEMP_ENVELOPE_MAX_MILLIC   ( 200000 )

/* Control-period bounds: this same task drives the overtemp/overcurrent/
 * fan-stall protection checks, so an unbounded period would silently
 * stretch every fault reaction. The upper bound keeps the scheduler load
 * sane while still allowing slow bench sweeps.
 *
 * The LOWER bound is a floor on the task's own RUN TIME, not a scheduling
 * preference: TecControl_Cb() performs three ADC conversions (hot, cold,
 * current) and each one blocks at least one millisecond inside
 * Studio_ADC1_ReadChannel()'s LL_mDelay(1) stabilisation, before any EOC
 * wait or the soft-I2C DAC write. A period shorter than the body's worst
 * case cannot be honoured - the task would overrun its slot, so the loop
 * would silently run at the body's rate while the PID, the I-term slew and
 * the fan-stall timers all still assume the configured one. TEC_PERIOD_MIN_MS
 * covers the three mandatory milliseconds plus the EOC waits and the DAC
 * transaction with margin; it is an order-of-magnitude floor, not a measured
 * worst case. Anything faster than this has no meaning on this hardware. */
#define TEC_PERIOD_MIN_MS              ( 10 )
#define TEC_PERIOD_MAX_MS              ( 5000 )

/* Physical-time constants for the PID housekeeping that used to be fixed
 * per-cycle numbers. The control period is NVRAM-tunable, so per-cycle values
 * silently change the loop's physical dynamics when the period changes:
 *   - D-term low-pass: first-order EMA with time constant TAU (s); the
 *     per-cycle smoothing factor is a = Ts/(TAU+Ts). At Ts = 1 s this gives
 *     a = 0.33, matching the previously hardcoded 0.3.
 *   - I-increment clamp: a physical integrator slew limit in %/s; the
 *     per-cycle clamp is +/-SLEW*Ts. At Ts = 1 s this gives +/-50, matching
 *     the previously hardcoded window. */
#define TEC_D_FILTER_TAU_S        ( 2.0f )
#define TEC_I_SLEW_PCT_PER_S      ( 50.0f )

/* Quiet NaN as a constant expression, for the "no gains applied yet" seed of
 * the PID-gain change detector (see s_pid_kp). A bit pattern is used instead of
 * math.h's NAN to avoid depending on a header in a static initialiser. */
#define FLOAT_NAN                 ( __builtin_nanf( "" ) )

/* Fan-stall protection: the tachometer RPM is the direct, fast indicator of a
 * dead heatsink fan, but it is a 2 s-window average (fan_task) that still
 * reads ~0 until the fan has spooled up and its first window completes.
 * Detection is therefore grace-gated and must be SUSTAINED before it may cut
 * the TEC:
 *   - TEC_FAN_GRACE_S: window ignored after a fresh fan start (spool-up +
 *     first tach sample), counted down per control cycle;
 *   - TEC_FAN_STALL_S: below-RPM time required before the TEC is cut
 *     (accumulated in ms, so the rule is independent of the control period). */
#define TEC_FAN_GRACE_S           ( 6u )
#define TEC_FAN_STALL_S           ( 5u )

/*============================================================================*/
/* Configuration: snapshotted once from NVRAM at TecControl_Init().            */
/*============================================================================*/
typedef struct {
    float   pid_kp;                 /* PID gains (float, Q16 in NVRAM)         */
    float   pid_ki;
    float   pid_kd;
    int32_t target_base_millic;     /* base TEC target temp                     */
    int32_t hot_overtemp_millic;    /* fan full speed threshold                 */
    int32_t hot_danger_millic;      /* TEC off + fan 100 % threshold            */
    int32_t cur_danger_ma;          /* overcurrent cutoff threshold             */
    int32_t fan_min_rpm;            /* fan fault RPM threshold                  */
    int32_t control_period_ms;      /* task period, ms                          */
    int32_t dac_max_ctrl_mv;        /* power-stage control window top           */
    int32_t fan_temp_base_millic;   /* fan ramp start (hot side)                */
    int32_t fan_temp_full_millic;   /* fan 100 % (hot side)                     */
    int32_t fan_duty_base;          /* base airflow while TEC active            */
    int32_t temp_valid_min_millic;  /* temperature plausibility window          */
    int32_t temp_valid_max_millic;
    int32_t current_valid_max_ma;   /* current plausibility window              */
    int32_t fan_fault_min_duty;     /* fan fault needs duty above this          */
    int32_t env_comp_temp_millic;   /* ambient above this raises the target     */
    int32_t soc_tighten_temp_millic;/* SoC above this tightens the target       */
    int32_t soc_tighten_delta_millic;/* how much it tightens by                  */
    int32_t dac_startup_pct;       /* first-enable startup point, % of full scale */
    int32_t en_off_pct;             /* duty below this drops TEC_VEN (anti-hiccup) */
    int32_t dac_min_ctrl_mv;        /* FB-injection window bottom (VFBTH point) */
    int32_t ramp_step_percent;      /* max duty change per cycle (soft start)   */
    int32_t dewpoint_margin_millic; /* target floor = dew point + this          */
    float   ff_gain;                /* COMMON-MODE feedforward %/°C (Q16 NVRAM) */
    uint8_t ff_enable;              /* feedforward master switch (0 = PID only) */
    uint32_t hot_fb_gain_q16;       /* hot foldback: cold-target lift per hot
                                       excess, Q16 (2.0 = +2 °C per 1 °C)      */
    int32_t hot_fb_max_millic;      /* hot foldback: max cold-target lift       */
} Tec_Params_t;

/* Envelope guard for a milli-degree parameter: return v when sane, else its
 * factory default (see the TEC_TEMP_ENVELOPE_* macro comment). */
static int32_t temp_param_or_default( int32_t v, uint8_t id )
{
    if ( ( v < TEC_TEMP_ENVELOPE_MIN_MILLIC ) ||
         ( v > TEC_TEMP_ENVELOPE_MAX_MILLIC ) ) {
        return (int32_t)NvParams_GetDefault( id );
    }
    return v;
}

static void tec_params_load( Tec_Params_t *p )
{
    p->pid_kp                = (float)NvParams_Get( NV_TEC_PID_KP_Q16 ) / NVM_Q16_SCALE;
    p->pid_ki                = (float)NvParams_Get( NV_TEC_PID_KI_Q16 ) / NVM_Q16_SCALE;
    p->pid_kd                = (float)NvParams_Get( NV_TEC_PID_KD_Q16 ) / NVM_Q16_SCALE;
    p->target_base_millic    = (int32_t)NvParams_Get( NV_TEC_TARGET_BASE_MILLIC );
    p->hot_overtemp_millic   = (int32_t)NvParams_Get( NV_TEC_HOT_OVERTEMP_MILLIC );
    p->hot_danger_millic     = (int32_t)NvParams_Get( NV_TEC_HOT_DANGER_MILLIC );
    p->cur_danger_ma         = (int32_t)NvParams_Get( NV_TEC_CURRENT_DANGER_MA );
    p->fan_min_rpm           = (int32_t)NvParams_Get( NV_TEC_FAN_MIN_RPM );
    p->control_period_ms     = (int32_t)NvParams_Get( NV_TEC_CONTROL_PERIOD_MS );
    p->dac_max_ctrl_mv       = (int32_t)NvParams_Get( NV_TEC_DAC_MAX_CTRL_MV );
    p->fan_temp_base_millic  = (int32_t)NvParams_Get( NV_TEC_FAN_TEMP_BASE_MILLIC );
    p->fan_temp_full_millic  = (int32_t)NvParams_Get( NV_TEC_FAN_TEMP_FULL_MILLIC );
    p->fan_duty_base         = (int32_t)NvParams_Get( NV_TEC_FAN_DUTY_BASE );
    p->temp_valid_min_millic = (int32_t)NvParams_Get( NV_TEC_TEMP_VALID_MIN_MILLIC );
    p->temp_valid_max_millic = (int32_t)NvParams_Get( NV_TEC_TEMP_VALID_MAX_MILLIC );
    p->current_valid_max_ma  = (int32_t)NvParams_Get( NV_TEC_CURRENT_VALID_MAX_MA );
    p->fan_fault_min_duty    = (int32_t)NvParams_Get( NV_TEC_FAN_FAULT_MIN_DUTY );
    p->env_comp_temp_millic  = (int32_t)NvParams_Get( NV_TEC_ENV_COMP_TEMP_MILLIC );
    p->soc_tighten_temp_millic = (int32_t)NvParams_Get( NV_TEC_SOC_TIGHTEN_TEMP_MILLIC );
    p->soc_tighten_delta_millic = (int32_t)NvParams_Get( NV_TEC_SOC_TIGHTEN_DELTA_MILLIC );
    p->dac_startup_pct        = (int32_t)NvParams_Get( NV_TEC_DAC_STARTUP_PCT );
    p->en_off_pct             = (int32_t)NvParams_Get( NV_TEC_EN_OFF_PCT );
    p->dac_min_ctrl_mv        = (int32_t)NvParams_Get( NV_TEC_DAC_MIN_CTRL_MV );
    p->ramp_step_percent      = (int32_t)NvParams_Get( NV_TEC_RAMP_STEP_PERCENT );
    p->dewpoint_margin_millic = (int32_t)NvParams_Get( NV_TEC_DEWPOINT_MARGIN_MILLIC );
    p->ff_gain                = (float)NvParams_Get( NV_TEC_FF_GAIN_Q16 ) / NVM_Q16_SCALE;
    p->ff_enable              = ( NvParams_Get( NV_TEC_FF_ENABLE ) != 0u ) ? 1u : 0u;
    p->hot_fb_gain_q16        = NvParams_Get( NV_TEC_HOT_FB_GAIN_Q16 );
    p->hot_fb_max_millic      = (int32_t)NvParams_Get( NV_TEC_HOT_FB_MAX_MILLIC );

    /* Absolute envelope on every milli-degree parameter (see the macro
     * comment). Runs BEFORE the relation checks below, so they only ever
     * compare bounded values - e.g. FAN_TEMP_BASE = 0x80000000 passes a
     * naive base<full relation but overflows the fan-ramp subtraction. */
    p->target_base_millic     = temp_param_or_default( p->target_base_millic, NV_TEC_TARGET_BASE_MILLIC );
    p->hot_overtemp_millic    = temp_param_or_default( p->hot_overtemp_millic, NV_TEC_HOT_OVERTEMP_MILLIC );
    p->hot_danger_millic      = temp_param_or_default( p->hot_danger_millic, NV_TEC_HOT_DANGER_MILLIC );
    p->fan_temp_base_millic   = temp_param_or_default( p->fan_temp_base_millic, NV_TEC_FAN_TEMP_BASE_MILLIC );
    p->fan_temp_full_millic   = temp_param_or_default( p->fan_temp_full_millic, NV_TEC_FAN_TEMP_FULL_MILLIC );
    p->env_comp_temp_millic   = temp_param_or_default( p->env_comp_temp_millic, NV_TEC_ENV_COMP_TEMP_MILLIC );
    p->soc_tighten_temp_millic = temp_param_or_default( p->soc_tighten_temp_millic, NV_TEC_SOC_TIGHTEN_TEMP_MILLIC );
    p->temp_valid_min_millic  = temp_param_or_default( p->temp_valid_min_millic, NV_TEC_TEMP_VALID_MIN_MILLIC );
    p->temp_valid_max_millic  = temp_param_or_default( p->temp_valid_max_millic, NV_TEC_TEMP_VALID_MAX_MILLIC );

    /* Sanity-check the DAC FB-injection window: tec_dac_set() computes
     * span = dac_max - dac_min and multiplies duty*span, so a corrupt
     * window (host-written NVRAM or a FLASH bit flip) where min >= max
     * would wrap span to ~2^32 and make the DAC output an arbitrary
     * voltage. Fall back to the factory defaults on any violation. All
     * fallbacks read s_defaults via NvParams_GetDefault() - a single
     * source of truth that follows the defaults table automatically
     * (NvParams_Get() itself is NOT usable here: it returns the very
     * cache values these checks just rejected). */
    if ( ( p->dac_min_ctrl_mv < 0 ) || ( p->dac_max_ctrl_mv < 0 ) ||
         ( p->dac_min_ctrl_mv > (int32_t)MCP4725_VDD_MV ) ||
         ( p->dac_max_ctrl_mv > (int32_t)MCP4725_VDD_MV ) ||
         ( p->dac_min_ctrl_mv >= p->dac_max_ctrl_mv ) ) {
        p->dac_min_ctrl_mv = (int32_t)NvParams_GetDefault( NV_TEC_DAC_MIN_CTRL_MV );
        p->dac_max_ctrl_mv = (int32_t)NvParams_GetDefault( NV_TEC_DAC_MAX_CTRL_MV );
    }

    /* Relation/range sanity for the rest of the parameter set (same threat
     * model: host-writable NVRAM or a FLASH bit flip). Each violated
     * invariant falls back to its factory default:
     *  - fan ramp span: full <= base divides by zero / reverses the ramp
     *  - hot thresholds: warning must stay below danger, or the warning
     *    path never fires before the danger shutdown
     *  - current thresholds: the FF knee must sit below the danger limit
     *  - PID gains: must be positive, or TecPid_Init() rejects them and
     *    the controller silently never runs
     *  - soft-start step: must be > 0, or the ramp never advances
     *  - plausibility window: min < max, or is_temp_valid() rejects
     *    everything and every cycle falls back to the target            */
    if ( p->fan_temp_base_millic >= p->fan_temp_full_millic ) {
        p->fan_temp_base_millic = (int32_t)NvParams_GetDefault( NV_TEC_FAN_TEMP_BASE_MILLIC );
        p->fan_temp_full_millic = (int32_t)NvParams_GetDefault( NV_TEC_FAN_TEMP_FULL_MILLIC );
    }
    if ( p->hot_overtemp_millic >= p->hot_danger_millic ) {
        p->hot_overtemp_millic = (int32_t)NvParams_GetDefault( NV_TEC_HOT_OVERTEMP_MILLIC );
        p->hot_danger_millic   = (int32_t)NvParams_GetDefault( NV_TEC_HOT_DANGER_MILLIC );
    }
    if ( p->cur_danger_ma <= 0 ) {
        p->cur_danger_ma = (int32_t)NvParams_GetDefault( NV_TEC_CURRENT_DANGER_MA );
    }
    if ( ( p->soc_tighten_temp_millic <= 0 ) ||
         ( p->soc_tighten_delta_millic <= 0 ) ||
         ( p->soc_tighten_delta_millic > TEC_TEMP_ENVELOPE_MAX_MILLIC ) ) {
        p->soc_tighten_temp_millic = (int32_t)NvParams_GetDefault( NV_TEC_SOC_TIGHTEN_TEMP_MILLIC );
        p->soc_tighten_delta_millic = (int32_t)NvParams_GetDefault( NV_TEC_SOC_TIGHTEN_DELTA_MILLIC );
    }
    if ( p->env_comp_temp_millic <= 0 ) {
        p->env_comp_temp_millic = (int32_t)NvParams_GetDefault( NV_TEC_ENV_COMP_TEMP_MILLIC );
    }
    if ( ( p->pid_kp <= 0.0f ) || ( p->pid_ki <= 0.0f ) || ( p->pid_kd < 0.0f ) ) {
        p->pid_kp = (float)NvParams_GetDefault( NV_TEC_PID_KP_Q16 ) / NVM_Q16_SCALE;
        p->pid_ki = (float)NvParams_GetDefault( NV_TEC_PID_KI_Q16 ) / NVM_Q16_SCALE;
        p->pid_kd = (float)NvParams_GetDefault( NV_TEC_PID_KD_Q16 ) / NVM_Q16_SCALE;
    }
    if ( ( p->ramp_step_percent <= 0 ) || ( p->ramp_step_percent > 100 ) ) {
        p->ramp_step_percent = (int32_t)NvParams_GetDefault( NV_TEC_RAMP_STEP_PERCENT );
    }
    if ( ( p->en_off_pct < 0 ) ||
         ( p->en_off_pct > ( 100 - (int32_t)TEC_EN_HYST_PCT ) ) ) {
        p->en_off_pct = (int32_t)NvParams_GetDefault( NV_TEC_EN_OFF_PCT );
    }
    if ( ( p->fan_duty_base < 0 ) || ( p->fan_duty_base > 100 ) ) {
        p->fan_duty_base = (int32_t)NvParams_GetDefault( NV_TEC_FAN_DUTY_BASE );
    }
    if ( ( p->fan_fault_min_duty < 0 ) || ( p->fan_fault_min_duty > 100 ) ) {
        /* The commanded duty never exceeds 100, so a threshold above it
         * makes the stall-detect condition unsatisfiable and fan-stall
         * protection silently dead. 0 stays legal: detection then runs at
         * any commanded duty (the duty>0 guard prevents the shutoff/
         * restart lockup). */
        p->fan_fault_min_duty = (int32_t)NvParams_GetDefault( NV_TEC_FAN_FAULT_MIN_DUTY );
    }
    if ( ( p->control_period_ms < TEC_PERIOD_MIN_MS ) ||
         ( p->control_period_ms > TEC_PERIOD_MAX_MS ) ) {
        p->control_period_ms = (int32_t)NvParams_GetDefault( NV_TEC_CONTROL_PERIOD_MS );
    }
    if ( ( p->dewpoint_margin_millic < 0 ) ||
         ( p->dewpoint_margin_millic > TEC_TEMP_ENVELOPE_MAX_MILLIC ) ) {
        p->dewpoint_margin_millic = (int32_t)NvParams_GetDefault( NV_TEC_DEWPOINT_MARGIN_MILLIC );
    }
    if ( p->temp_valid_min_millic >= p->temp_valid_max_millic ) {
        p->temp_valid_min_millic = (int32_t)NvParams_GetDefault( NV_TEC_TEMP_VALID_MIN_MILLIC );
        p->temp_valid_max_millic = (int32_t)NvParams_GetDefault( NV_TEC_TEMP_VALID_MAX_MILLIC );
    }
    if ( p->current_valid_max_ma <= 0 ) {
        p->current_valid_max_ma = (int32_t)NvParams_GetDefault( NV_TEC_CURRENT_VALID_MAX_MA );
    }
    if ( p->current_valid_max_ma < p->cur_danger_ma ) {
        /* The plausibility window must sit ABOVE the danger threshold, or a
         * real overcurrent inside [danger, valid_max) gets mislabeled and
         * handled as a sensor fault instead of the OVERCURRENT path. */
        p->current_valid_max_ma = (int32_t)NvParams_GetDefault( NV_TEC_CURRENT_VALID_MAX_MA );
        p->cur_danger_ma        = (int32_t)NvParams_GetDefault( NV_TEC_CURRENT_DANGER_MA );
    }
    if ( ( p->hot_fb_gain_q16 == 0u ) || ( p->hot_fb_max_millic <= 0 ) ||
         ( p->hot_fb_max_millic > TEC_TEMP_ENVELOPE_MAX_MILLIC ) ) {
        p->hot_fb_gain_q16   = NvParams_GetDefault( NV_TEC_HOT_FB_GAIN_Q16 );
        p->hot_fb_max_millic = (int32_t)NvParams_GetDefault( NV_TEC_HOT_FB_MAX_MILLIC );
    }
}

/*============================================================================*/
/* Local state                                                                 */
/*============================================================================*/
static qTask_t     s_task;
static TecPid_t    s_pid;
static Tec_Params_t s_p;
static uint8_t     s_alarm;
static uint8_t     s_tec_enabled;
static uint8_t     s_dac_percent = 0xFFu; /* last duty written to the DAC;
                                           0xFF = "unknown" sentinel so the
                                           first tec_dac_set() is never
                                           short-circuited                    */
#if ( TEC_FAN_FAULT_ENABLE == 1 )
static uint32_t    s_fan_stall_ms;  /* accumulated below-RPM time            */
static uint32_t    s_fan_grace_ms;  /* remaining fan spool-up grace          */
static uint8_t     s_fan_stalled;   /* interlock: only healthy RPM clears it */
static uint8_t     s_fan_started;   /* do not renew grace on each TEC enable */
#endif /* TEC_FAN_FAULT_ENABLE */
static uint8_t     s_dew_hold; /* latched low-output mode, 1 C release band */
static uint8_t     s_cold_fault;    /* reset PID from real feedback on recovery */
static volatile int32_t hot_mc;
static volatile int32_t env_mc;
static volatile int32_t sdm_mc;
static volatile uint32_t cur_ma;
static volatile int32_t target_mc;
static volatile int32_t duty;

/* Live parameter reload bookkeeping.
 *
 * The snapshot in s_p is refreshed whenever the NVRAM cache generation moves
 * (see NvParams_GetCacheGeneration), so a host write to the I2C parameter
 * window takes effect on the next control cycle instead of requiring a
 * reboot. Two quantities are deliberately NOT refreshed:
 *
 *   - the loop period: it is fixed by the QuarkTS schedule created at init,
 *     and it normalizes the PID's ki/kd and the fan-stall timers. A host write
 *     to NV_TEC_CONTROL_PERIOD_MS cannot change the running schedule, so the
 *     boot value is authoritative - using the newly written value would
 *     silently rescale every physical gain;
 *   - the PID gains: applied only when the GAIN VALUES change, never merely
 *     because some other slot was written (TecPid_Init clears the integrator
 *     and history - see tec_apply_pid_params()). */
static int32_t     s_loop_period_ms = 1000;  /* effective loop period, set once */
static uint32_t    s_nv_gen;        /* s_cache_gen s_p was last loaded from      */

/* The gains currently loaded in s_pid. Initialised to NaN so the first
 * comparison is always unequal (NaN != NaN) and the controller is set up
 * regardless of which gain value is in NVRAM. */
static float       s_pid_kp = FLOAT_NAN;
static float       s_pid_ki = FLOAT_NAN;
static float       s_pid_kd = FLOAT_NAN;

/* (Re-)apply the parameter snapshot to the PID controller.
 *
 * The sample period is the boot-time loop period, NOT s_p.control_period_ms:
 * a host write to NV_TEC_CONTROL_PERIOD_MS cannot change the running QuarkTS
 * schedule, and ki/kd are physical per-second gains that this period scales -
 * honouring a mid-run write here would silently rescale them. Boot-time-only
 * is therefore the honest mapping (see s_loop_period_ms).
 *
 * The decision to re-initialise is made on the GAIN VALUES, not on the NVRAM
 * cache generation: TecPid_Init() clears the integrator and history, and every
 * parameter write (including slots with nothing to do with the PID) moves the
 * generation. Keying off the generation would therefore reset a running
 * controller on any unrelated write - exactly what this guard exists to
 * prevent. Re-initialising on a genuine gain change IS wanted: the alternative
 * is quietly running the new gains against the old tuning. */
static void tec_apply_pid_params( void )
{
    float period_s;

    if ( ( s_p.pid_kp == s_pid_kp ) &&
         ( s_p.pid_ki == s_pid_ki ) &&
         ( s_p.pid_kd == s_pid_kd ) ) {
        return;
    }

    period_s = (float)s_loop_period_ms / 1000.0f;

    (void)TecPid_Init( &s_pid,
                       s_p.pid_kp, s_p.pid_ki, s_p.pid_kd,
                       period_s,
                       -(float)s_p.target_base_millic,
                       0.0f, 100.0f,
                       -( TEC_I_SLEW_PCT_PER_S * period_s ),
                       ( TEC_I_SLEW_PCT_PER_S * period_s ),
                       period_s / ( TEC_D_FILTER_TAU_S + period_s ) );
    s_pid_kp = s_p.pid_kp;
    s_pid_ki = s_p.pid_ki;
    s_pid_kd = s_p.pid_kd;
}

/* Refresh the parameter snapshot when the NVRAM cache has moved, then re-apply
 * the PID gains if they are among the changed values. Called once per control
 * cycle, before anything reads s_p, so every decision in that cycle uses one
 * consistent, validated parameter set. */
static void tec_params_reload_if_needed( void )
{
    uint32_t gen = NvParams_GetCacheGeneration();

    if ( gen == s_nv_gen ) {
        return;
    }

    s_nv_gen = gen;
    tec_params_load( &s_p );

    /* The running schedule owns the period, so a value written over the I2C
     * window must not reach the fan-stall timers - they accumulate in ms and
     * would otherwise measure against a period the loop does not run at. */
    s_p.control_period_ms = s_loop_period_ms;

    tec_apply_pid_params();
}

/*============================================================================*/
/* Helpers                                                                     */
/*============================================================================*/
static int is_temp_valid( int32_t millic )
{
    return ( millic >= s_p.temp_valid_min_millic ) &&
           ( millic <= s_p.temp_valid_max_millic );
}

/* Set the TEC enable line (PA12): non-zero enables. */
static void tec_set_enabled( uint8_t on )
{
    if ( on != 0u ) {
        LL_GPIO_SetOutputPin( GPIOA, TEC_VEN_Pin );
    } else {
        LL_GPIO_ResetOutputPin( GPIOA, TEC_VEN_Pin );
    }
    s_tec_enabled = ( on != 0u ) ? 1u : 0u;
}

/* Command the TEC current via the MCP4725: cooling duty (0..100 %) is mapped
 * onto the power-stage FB-injection window. The DAC injects current into the
 * TPS565201 FB node through R3 (standard FB-injection topology): a higher DAC
 * voltage pulls FB up and the converter output goes DOWN. So the TEC output
 * voltage is INVERSELY related to the DAC voltage and the mapping below is
 * inverted on purpose:
 *   duty 0%   (no cooling / TEC off)  -> DAC dac_max_ctrl_mV -> VOUT pulled low
 *   duty 100% (full cooling)          -> DAC dac_min_ctrl_mV -> VOUT = divider
 *                                        natural set point (VFBTH, e.g. 6 V)
 * dac_min_ctrl_mV is the mV at which FB sits at VFBTH (0.76 V): with no
 * injection current the divider alone sets the max TEC voltage there. Never
 * command below it - with FB below the reference the D-CAP2 converter
 * over-drives toward max duty (uncontrolled TEC voltage / UVP hiccup). Both
 * endpoints are NVRAM calibration points (see R3 sizing in the WORKLOG).
 *
 * NOTE on startup: the anchor sits at the LOW end (DAC_STARTUP_PCT, default
 * 10 % of full cooling ≈ 0.6 V under the calibrated window [760, 2500]) and
 * the per-cycle ramp climbs from there - true low-voltage soft start, inrush
 * bounded by RAMP_STEP_PERCENT. Because the window bottom IS the no-injection
 * point (760 mV = FB at VREF), mv can never fall into the FB<VREF region, so
 * the enable path can no longer reach the UVP (<494 mV) or overdrive (>6 V)
 * regions; the TPS565201's 1 ms internal soft start only covers the initial
 * 0 -> anchor step.
 *
 * Failure handling: the DAC write status is returned; s_dac_percent is only
 * advanced on SUCCESS, so a failed write never pretends the value was applied
 * (a stale DAC output while the control loop thinks it moved the TEC voltage
 * would be a silent mis-actuation). Callers that gate TEC enable on this must
 * check the return. */
/* Force a real DAC write even when `duty` equals the cached value: the cache
 * can diverge from the physical DAC (the boot "unknown" sentinel, a
 * previously failed write, or the MCP4725 falling back to its EEPROM
 * power-up value after a brown-out). Callers that must GUARANTEE a bus
 * transaction (the first-enable anchor before TEC_VEN) go through here.
 * s_dac_percent advances only on SUCCESS, so a failed forced write leaves
 * the cache pointing at the last value known to be on the DAC. */
static int tec_dac_set_forced( uint8_t duty )
{
    uint16_t mv;
    uint32_t span;
    MCP4725_Status_t st;

    span = (uint32_t)( s_p.dac_max_ctrl_mv - s_p.dac_min_ctrl_mv );
    mv = (uint16_t)( (uint32_t)s_p.dac_max_ctrl_mv -
                     ( ( (uint32_t)duty * span ) / 100u ) );
    st = MCP4725_SetVoltage_mV( mv );
    if ( st == MCP4725_OK ) {
        s_dac_percent = duty;
    }
    return (int)st;
}

static int tec_dac_set( uint8_t duty )
{
    if ( duty == s_dac_percent ) {
        return MCP4725_OK;
    }
    return tec_dac_set_forced( duty );
}

/* Heatsink fan is decoupled from the TEC PID output: base airflow while the
 * TEC is active, then a linear ramp over [fan_temp_base, fan_temp_full] on the
 * hot-side temperature, saturating at 100 %. */
static uint8_t tec_fan_duty_from_hot( int32_t hot_mc )
{
    int32_t duty = s_p.fan_duty_base;

    if ( hot_mc > s_p.fan_temp_base_millic ) {
        duty = s_p.fan_duty_base +
               ( ( hot_mc - s_p.fan_temp_base_millic ) *
                 ( 100 - s_p.fan_duty_base ) ) /
               ( s_p.fan_temp_full_millic - s_p.fan_temp_base_millic );
    }
    if ( duty > 100 ) {
        duty = 100;
    }
    if ( duty < 0 ) {
        duty = 0;
    }
    return (uint8_t)duty;
}

/*============================================================================*/
/* Periodic control callback                                                   */
/*============================================================================*/
static void TecControl_Cb( qEvent_t e )
{

    int32_t hot_lift;
    int32_t pid_out;
    int32_t feedforward;

    uint8_t fan_duty;
#if ( TEC_FAN_FAULT_ENABLE == 1 )
    uint32_t fan_rpm;
#endif
    uint32_t cur_read;
    uint8_t  cur_ok;
    uint8_t dew_was_hold;
    int32_t dew_min_duty;           /* derived from s_p: set AFTER the reload */
    uint8_t drive;                  /* 0 = hold the converter off this cycle */

    (void)e;

    /* Alarms report THIS cycle's conditions: clear first, so the early
     * fail-safe returns below cannot carry stale bits from previous cycles
     * (e.g. an OVERCURRENT or DAC_FAULT whose condition has since gone). */
    s_alarm = TEC_ALARM_NONE;

    /*--- 0. pick up parameter writes made over the I2C window --------------*/
    /* Host-tunable values take effect from the NEXT cycle; nothing below reads
     * a parameter from any other source, so this cycle stays self-consistent.
     * Everything derived from s_p must therefore be computed AFTER this call -
     * a value hoisted into the declarations above (as dew_min_duty once was)
     * would silently mix the previous cycle's EN_OFF_PCT with this cycle's
     * deadband, target and fan thresholds. */
    tec_params_reload_if_needed();

    /* Minimum cooling held during a dew-point event: just above the EN
     * deadband exit, so the hold is never mistaken for the anti-hiccup cut. */
    dew_min_duty = s_p.en_off_pct + TEC_EN_HYST_PCT;

    /*--- 1. read all sources ------------------------------------------------*/
    hot_mc = Studio_ADC1_ReadHotMilliCelsius();
    env_mc = AHT20_Get_LastTempMilliCelsius();   /* ambient, not dew point */
    sdm_mc = Studio_ADC1_ReadColdMilliCelsius();
    cur_ok = Studio_ADC1_ReadCurrentMilliamp( &cur_read );
    cur_ma = cur_read;

    if ( is_temp_valid( env_mc ) == 0 ) {
        env_mc = s_p.target_base_millic;   /* fall back to nominal ambient */
    }
    if ( is_temp_valid( sdm_mc ) == 0 ) {
        tec_set_enabled( 0u );
        (void)tec_dac_set( 0u );
        Fan_SetEnabled( 1u );
        Fan_SetDutyPercent( 100u );
        s_cold_fault = 1u;
        s_alarm |= TEC_ALARM_SENSOR;
        return;
    }
    if ( s_cold_fault != 0u ) {
        TecPid_Reset( &s_pid, -(float)sdm_mc );
        s_cold_fault = 0u;
    }

    /*--- 1b. hot-side sensor failure is FAIL-SAFE, not fall-back -----------*/
    /* Only ambient falls back to the target on an invalid
     * reading; the hot side CANNOT - it feeds the overtemp shutdown, the
     * fan ramp and the fan-fault check. An open NTC reads as -40 C (would
     * never trip the overtemp guard) and a shorted NTC as +200 C. Treat
     * any invalid hot-side reading as a sensor fault: cut the TEC, spin the
     * fan at 100 % and stop driving until the reading is plausible again.
     * (ADC read timeout also surfaces here, via an out-of-window code.) */
    if ( is_temp_valid( hot_mc ) == 0 ) {
        tec_set_enabled( 0u );
        (void)tec_dac_set( 0u );
        Fan_SetDutyPercent( 100u );
        s_alarm |= TEC_ALARM_SENSOR;
        return;
    }

    /*--- 1c. current-sense failure is FAIL-SAFE, not fall-back -------------*/
    /* A timed-out ADC conversion reads as code 0, which in the mA domain is
     * a LEGITIMATE value (TEC off) - adc.c therefore reports sample validity
     * explicitly instead of letting 0 stand in for "no data". With a failed
     * sample the overcurrent guard is BLIND: cut the TEC (same shape as the
     * hot-side sensor fault above) until a valid sample returns; recovery is
     * automatic through the normal first-enable path. The second half is the
     * NVRAM plausibility window (current_valid_max_ma - previously loaded
     * and validated but never consulted): a reading above it means the sense
     * chain itself is no longer trustworthy. danger < valid_max is enforced
     * in tec_params_load(): readings between danger and valid_max use the
     * OVERCURRENT path; readings beyond valid_max use this SENSOR path. */
    if ( ( cur_ok == 0u ) ||
         ( cur_ma > (uint32_t)s_p.current_valid_max_ma ) ) {
        tec_set_enabled( 0u );
        (void)tec_dac_set( 0u );
        Fan_SetEnabled( 1u );
        Fan_SetDutyPercent( 100u );
        TecPid_Reset( &s_pid, -(float)sdm_mc );
        s_alarm |= TEC_ALARM_SENSOR;
        if ( hot_mc > s_p.hot_danger_millic ) s_alarm |= TEC_ALARM_OVERTEMP;
        return;
    }

    /*--- 2. target temperature --------------------------------------------*/
    target_mc = s_p.target_base_millic;
    if ( env_mc > s_p.env_comp_temp_millic ) {
        target_mc += ( env_mc - s_p.env_comp_temp_millic ) / 2;  /* +0.5°C per °C above knee */
    }
    if ( sdm_mc > s_p.soc_tighten_temp_millic ) {
        target_mc -= s_p.soc_tighten_delta_millic;
    }
    /*--- 2b. hot-side foldback: lift the cold target to shed TEC load ------*/
    /* A TEC pumps waste heat into its hot side, so the way to relieve an
     * overheating heatsink is to cool LESS, not harder. Instead of dropping
     * the duty to 0 (bang-bang: cold-side rebounds, then the re-enabled TEC
     * slams back and the cycle repeats), lift the cold-side target above
     * its base by a gain proportional to the hot excess and let the PID
     * glide the duty down smoothly. Recovery is automatic: once the hot
     * side cools below the knee the lift decays back to zero. */
    hot_lift = 0;
    if ( hot_mc > s_p.hot_overtemp_millic ) {
        int32_t excess = hot_mc - s_p.hot_overtemp_millic;
        int64_t lift;

        lift = ( (int64_t)excess * (int64_t)s_p.hot_fb_gain_q16 ) >> 16;
        if ( lift > s_p.hot_fb_max_millic ) {
            lift = s_p.hot_fb_max_millic;
        }
        if ( lift < 0 ) {
            lift = 0;
        }
        target_mc += (int32_t)lift;
        hot_lift = (int32_t)lift;
    }

    /*--- 4. fault protection (highest priority first) ----------------------*/

    if ( hot_lift > 0 ) {
        /* Foldback active: the elevated cold target is doing the load
         * shedding - keep the PID running (no duty clamp, no integrator
         * reset), just report the derating to the host. */
        s_alarm |= TEC_ALARM_HOT_WARNING;
    }

    /* Dew protection derates rather than toggling EN. Keep the requested
     * target above dew + margin, and latch minimum cooling until the cold
     * side has warmed 1 C above that floor. An unavailable humidity sample
     * must not silently restore unrestricted cooling. */
    dew_was_hold = s_dew_hold;
    {
        int32_t dew_mc = AHT20_Get_LastDewPointMilliCelsius();
        if ((dew_mc < TEC_TEMP_ENVELOPE_MIN_MILLIC) ||
            (dew_mc > TEC_TEMP_ENVELOPE_MAX_MILLIC)) {
            s_dew_hold = 1u;
            s_alarm |= TEC_ALARM_SENSOR;
        } else {
            int32_t floor_mc = dew_mc + s_p.dewpoint_margin_millic;
            if (target_mc < floor_mc) target_mc = floor_mc;
            if (sdm_mc <= floor_mc) s_dew_hold = 1u;
            else if (sdm_mc >= floor_mc + TEC_DEW_RELEASE_MILLIC) s_dew_hold = 0u;
        }
        if (s_dew_hold) s_alarm |= TEC_ALARM_DEWPOINT;
    }

    if ( hot_mc > s_p.hot_danger_millic ) {
        /* danger: cut TEC, hold the DAC at the zero-current end (window top),
         * fan full speed. NOTE: tec_dac_set(0) would mean FULL COOLING in
         * the inverted mapping below - the off/zero-current direction is
         * dac_max_ctrl_mv, so send duty=0 only after the mapping flip. */
        tec_set_enabled( 0u );
        (void)tec_dac_set( 0u );   /* 0 duty = 0 cooling = DAC at max_ctrl (VOUT 0) */
        Fan_SetDutyPercent( 100u );
        s_alarm |= ( TEC_ALARM_OVERTEMP | TEC_ALARM_HOT_WARNING );
        return;
    }

    /* Thermal precheck gate: the SDM845 boot state machine cools the enclosure
     * (fan full speed) until the AHT20 ambient is safe before starting the SoC
     * and TEC. While that is pending, hold the TEC off (DAC at the zero-current
     * end, VOUT ~ 0) and do not drive the fan (the precheck owns it), so we
     * never fight the cooldown. */
    if ( Sdm845_Boot_IsThermalReady() == 0u ) {
        tec_set_enabled( 0u );
        (void)tec_dac_set( 0u );   /* 0 duty = 0 cooling = DAC at max_ctrl (VOUT 0) */
        s_alarm |= TEC_ALARM_THERMAL_WAIT;
        return;
    }

#if ( TEC_FAN_FAULT_ENABLE == 1 )
    /* A stalled fan must recover before ANY enable write. Alarm clearing
     * by the host cannot clear this interlock. Keep requesting full airflow. */
    if ( s_fan_stalled != 0u ) {
        if ( Fan_Get_LastRPM() < (uint32_t)s_p.fan_min_rpm ) {
            tec_set_enabled( 0u );
            Fan_SetEnabled( 1u );
            Fan_SetDutyPercent( 100u );
            s_alarm |= TEC_ALARM_FAN_FAULT;
            return;
        }
        s_fan_stalled = 0u;
        s_fan_stall_ms = 0u;
        TecPid_Reset( &s_pid, -(float)sdm_mc );
    }
#endif /* TEC_FAN_FAULT_ENABLE */

    /*--- 5. PID main loop --------------------------------------------------*/
    /* The controlled variable is the COLD side (sdm_mc); the hot side and
     * ambient never enter the PID error. EPID is "heating semantics"
     * (error = SP - x, output increases when temperature is below setpoint).
     * Fan is a cooling actuator (output increases when temperature is above
     * setpoint), so we invert both setpoint and measure to flip the effective
     * error to (x - SP). */
    if (s_dew_hold) {
        /* Freeze accumulation while the actuator is externally constrained. */
        TecPid_TrackOutput(&s_pid, -(float)sdm_mc, (float)dew_min_duty);
        pid_out = dew_min_duty;
    } else {
        if (dew_was_hold) {
            float actual = (s_tec_enabled && s_dac_percent <= 100u) ?
                           (float)s_dac_percent : 0.0f;
            TecPid_TrackOutput(&s_pid, -(float)sdm_mc, actual);
        }
        TecPid_SetSetpoint( &s_pid, -(float)target_mc );
        TecPid_Update( &s_pid, -(float)sdm_mc );
        pid_out = (int32_t)TecPid_GetOutput( &s_pid );
    }

    /*--- 5b. COMMON-MODE feedforward (master switch NV_TEC_FF_ENABLE,
     * default OFF) ----------------------------------------------------------*/
    /* The FF is ADDITIVE outside the PID's [0,100] domain: when it
     * overestimates the steady-state need, duty can never fall below it (the
     * PID is pinned at out_min and cannot subtract), so the cold side settles
     * away from the target - observed on the bench as "the output never
     * settles at target". The switch lets the bench A/B the FF without a
     * rebuild; re-enable once the FF estimate is calibrated (or the FF is
     * folded into the PID's own output limits so anti-windup sees the true
     * saturation).
     * NOTE: this is the ONLY feedforward term. The former current-derived
     * term (duty += (cur_mA - knee)/10) was REMOVED: with duty% == TEC
     * voltage %, dI/dduty ~= 25 mA/% gives that path a POSITIVE loop gain of
     * ~2.5 (>1) inside the 2000-3000 mA band - duty rises, current rises,
     * the term adds more duty, and the only brake was the overcurrent cutoff
     * (limit cycle up to the danger threshold). TEC current now serves
     * protection only (overcurrent cutoff + validity window). */
    feedforward = 0;
    if ( (s_p.ff_enable != 0u) && !s_dew_hold ) {
        {
            int32_t gradient_mc = env_mc - target_mc;   /* >0 = hotter ambient  */
            int32_t gradient_c  = gradient_mc / 1000;    /* milli-degC -> degC  */
            int32_t ff_base     = (int32_t)( gradient_c * s_p.ff_gain );

            if ( ff_base < 0 ) {
                ff_base = 0;
            }
            if ( ff_base > 100 ) {
                ff_base = 100;
            }
            feedforward = ff_base;
        }
    }
    if ( cur_ma > (uint32_t)s_p.cur_danger_ma ) {
        /* Overcurrent: cutting COOLING, not adding it. The old
         * `feedforward += 50` was a positive-feedback loop: higher current
         * -> higher duty -> more TEC current -> trip the power stage. Force
         * the TEC drive to 0 (stop cooling) AND drop the enable line, so a
         * never-decaying overcurrent (short, PSU fault) does not keep the
         * power stage driving a suspected-short load. Recovery goes through
         * the normal startup path (FB anchor + ramp) next cycle. */
        s_alarm |= TEC_ALARM_OVERCURRENT;
        tec_set_enabled( 0u );
        TecPid_Reset( &s_pid, -(float)sdm_mc );
        duty = 0;
        (void)tec_dac_set( 0u );   /* immediate: drop TEC drive this cycle */
        Fan_SetDutyPercent( 100u );   /* keep the heatsink at full airflow */
        return;              /* wait for current to fall below limit */
    }

    /*--- 7. combine, clamp --------------------------------------------------*/
    duty = pid_out + feedforward;   /* ff = 0 when NV_TEC_FF_ENABLE is off */
    if ( duty > 100 ) {
        duty = 100;
    }
    if ( duty < 0 ) {
        duty = 0;
    }

    /*--- 7b. EN deadband (anti-hiccup) --------------------------------------*/
    /* A high DAC (low duty) pulls VOUT down hard; below the converter's UVP
     * boundary the TPS565201 drops into hiccup mode (periodic restarts) and
     * the inductor audibly whines. Deep-light-load demand is therefore served
     * by DROPPING TEC_VEN (the converter EN) instead: zero TEC current, no
     * hiccup, and the cold side warms back up - which also keeps it from
     * hanging below the target. Hysteresis (TEC_EN_HYST_PCT) keeps the
     * boundary from chattering:
     *   EN on  + duty < off             -> EN off, DAC parked, wait
     *   EN off + duty < off + hysteresis -> hold off (no re-enable chatter)
     *   EN off + duty >= off + hysteresis -> normal first-enable path below
     *   EN on  + duty in the band        -> keep driving (hysteresis band) */
    drive = 1u;
    if ( s_tec_enabled != 0u ) {
        if ( duty < s_p.en_off_pct ) {
            tec_set_enabled( 0u );
            (void)tec_dac_set( 0u );   /* park at the off end while disabled */
            drive = 0u;
        }
    } else {
        if ( duty < ( s_p.en_off_pct + (int32_t)TEC_EN_HYST_PCT ) ) {
            drive = 0u;   /* hold off; the DAC is already parked */
        }
    }

    if ( ( s_tec_enabled == 0u ) && ( drive != 0u ) ) {
        /* First enable: settle the FB node at the startup anchor BEFORE
         * asserting TEC_VEN, then let the ramp climb.
         *
         * The anchor is a PERCENT of full cooling (DAC_STARTUP_PCT, default
         * 10): under the calibrated window [760, 2500] duty% maps linearly to
         * TEC voltage (% of the 6 V rating), so 10 % starts at ≈0.6 V and the
         * per-cycle ramp (RAMP_STEP_PERCENT) climbs to the operating point -
         * inrush is bounded by the ramp, not by a voltage jump. With the
         * window bottom at the no-injection point (760 mV) mv can never drop
         * below FB=VREF, so this path can no longer land in the UVP (<494 mV)
         * or overdrive (>6 V) regions. Reached only when the duty demand has
         * cleared the EN deadband (off + hysteresis), so the converter never
         * switches on into the hiccup region. */
        {
            uint16_t start_duty;
            int dac_st;

            if ( s_p.dac_startup_pct >= 100 ) {
                start_duty = 100u;
            } else if ( s_p.dac_startup_pct <= 0 ) {
                start_duty = 0u;
            } else {
                start_duty = (uint16_t)s_p.dac_startup_pct;
            }
            if (s_dew_hold) start_duty = (uint16_t)dew_min_duty;
            /* Forced write: the cache may not reflect the physical DAC here
             * (an earlier failed park, or an MCP4725 brown-out back to its
             * EEPROM power-up value), and with DAC_STARTUP_PCT == 0 a cached
             * equality check would silently skip the anchor entirely -
             * asserting TEC_VEN against an unverified DAC. Send the anchor
             * unconditionally; failure latches TEC_ALARM_DAC_FAULT below and
             * keeps the TEC off. */
            dac_st = tec_dac_set_forced( start_duty );   /* settle FB before enabling */
            if ( dac_st != MCP4725_OK ) {
                /* DAC write failed: DO NOT enable the TEC - the FB node was
                 * never settled to the startup anchor, so powering the
                 * converter now could drive it to max duty / hiccup. Keep the
                 * TEC disabled and retry next cycle. */
                s_alarm |= TEC_ALARM_DAC_FAULT;
                tec_set_enabled( 0u );
                return;
            }
        }
        tec_set_enabled( 1u );   /* demand cleared the deadband: TEC on */

        /* Take ownership of the fan PWM channel. Fan_Init() defaults the
         * FAN_CH_SW source switch to the SDM845 (the SoC drives the PWM), so
         * writing TIM3 CH2 duty alone is a no-op until we flip ownership to
         * the PY32. Without this the TEC loop's fan ramps (full speed on
         * overtemp, hot-side following) never reach the fan. */
        Fan_SetEnabled( 1u );
#if ( TEC_FAN_FAULT_ENABLE == 1 )
        /* Fresh fan start: arm the spool-up grace and clear the stall timer
         * so the first tach windows (which still read ~0 RPM) cannot trip
         * the sustained-stall cut. */
        if ( s_fan_started == 0u ) {
            s_fan_grace_ms = TEC_FAN_GRACE_S * 1000u;
            s_fan_stall_ms = 0u;
            s_fan_started = 1u;
        }
#endif /* TEC_FAN_FAULT_ENABLE */
    }

    /*--- 7c. soft-start ramp (only while driving) ---------------------------*/
    /* Soft-start: never step the TEC duty by more than ramp_step_percent per
     * cycle. The startup anchor (NV_TEC_DAC_STARTUP_PCT, % of full scale) is
     * where the TEC control starts from after enable - under the calibrated
     * window duty% == TEC voltage %, so a low anchor plus this ramp bounds the
     * inrush and avoids slamming the TEC (TI SNVAA70 §3.3 "start close to Vin
     * and regulate slowly"). */
    if ( drive != 0u ) {
        if ( duty > ( (int32_t)s_dac_percent + s_p.ramp_step_percent ) ) {
            duty = (int32_t)s_dac_percent + s_p.ramp_step_percent;
        }
        if ( duty < ( (int32_t)s_dac_percent - s_p.ramp_step_percent ) ) {
            duty = (int32_t)s_dac_percent - s_p.ramp_step_percent;
        }
        if ( duty < 0 ) {
            duty = 0;
        }
        if (s_dew_hold && duty < dew_min_duty) duty = dew_min_duty;
    }

    /*--- 8. fan stall detection (sustained stall cuts the TEC) --------------*/
    /* The tachometer is the direct, fast indicator of a dead heatsink fan;
     * waiting for the hot side to climb into foldback/danger (80/90 degC)
     * wastes that lead time. Rules:
     *   - judge only when real airflow is commanded: duty > 0 AND at or above
     *     the NVRAM detection threshold (>= so the base airflow is covered);
     *   - the alarm bit asserts whenever the condition holds (s_alarm is
     *     recomputed every cycle, so the host sees a persistent warning);
     *   - while the condition holds the fan is driven at 100 %: the hot-side
     *     ramp is meaningless without airflow feedback and max drive gives
     *     a marginal fan the best chance to start;
     *   - the TEC is cut only after the stall persists TEC_FAN_STALL_S
     *     beyond the spool-up grace - one slow tach window or a brief sag
     *     must not kill the cooling;
     *   - recovery is automatic: a healthy RPM sample clears the timer and
     *     the TEC re-enables through the normal deadband/first-enable path. */
    fan_duty = tec_fan_duty_from_hot( hot_mc );

#if ( TEC_FAN_FAULT_ENABLE == 1 )
    /* Tachometer-based stall detection (see rules above): the hot-side ramp
     * below is bypassed while a stall holds (fan driven at 100 %), and a
     * sustained stall cuts the TEC. Compiled out when TEC_FAN_FAULT_ENABLE
     * is 0 (no fan fitted on the bench: RPM reads 0 forever, which would
     * otherwise latch FAN_FAULT and block the first-enable interlock). */
    fan_rpm  = Fan_Get_LastRPM();

    if ( s_fan_grace_ms != 0u ) {
        /* Unconditional per-cycle countdown - decrementing only inside the
         * fault branch would grant a fresh grace at every later stall. */
        s_fan_grace_ms = ( s_fan_grace_ms > (uint32_t)s_loop_period_ms )
                         ? ( s_fan_grace_ms - (uint32_t)s_loop_period_ms )
                         : 0u;
    }

    if ( ( fan_duty > 0u ) &&
         ( fan_duty >= (uint8_t)s_p.fan_fault_min_duty ) &&
         ( fan_rpm < (uint32_t)s_p.fan_min_rpm ) ) {
        s_alarm |= TEC_ALARM_FAN_FAULT;
        fan_duty = 100u;   /* stalled: hot-follow ramp is meaningless, max drive */

        if ( s_fan_grace_ms == 0u ) {
            s_fan_stall_ms += (uint32_t)s_loop_period_ms;
            if ( s_fan_stall_ms >= ( TEC_FAN_STALL_S * 1000u ) ) {
                /* Sustained stall: the heatsink has no airflow, so drop the
                 * TEC now instead of baking it until the hot side trips. */
                if ( s_tec_enabled != 0u ) {
                    tec_set_enabled( 0u );
                    TecPid_Reset( &s_pid, -(float)sdm_mc );
                    (void)tec_dac_set( 0u );
                }
                Fan_SetDutyPercent( 100u );
                s_fan_stalled = 1u; /* held until a healthy RPM sample */
                return;
            }
        }
    } else {
        s_fan_stall_ms = 0u;   /* healthy sample: restart the stall timer */
    }
#endif /* TEC_FAN_FAULT_ENABLE */

    Fan_SetDutyPercent( fan_duty );        /* heatsink fan: follows hot side */
    if ( drive != 0u ) {
        if ( tec_dac_set( (uint8_t)duty ) != MCP4725_OK ) {
            /* Disable independently of the failed bus. Recovery must pass
             * the forced startup-anchor write before asserting EN again. */
            tec_set_enabled( 0u );
            TecPid_Reset( &s_pid, -(float)sdm_mc );
            Fan_SetDutyPercent( 100u );
            s_alarm |= TEC_ALARM_DAC_FAULT;
        }
    }
}

/*============================================================================*/
/* Public API                                                                  */
/*============================================================================*/
void TecControl_Init( void )
{
    float period_s;

    tec_params_load( &s_p );

    if ( s_p.control_period_ms < 1 ) {
        s_p.control_period_ms = 1000;   /* sanity: never schedule faster than 1 ms */
    }
    /* The loop period is fixed here, once: it defines the QuarkTS schedule
     * created below AND normalizes the PID's ki/kd. A later host write to
     * NV_TEC_CONTROL_PERIOD_MS cannot change the running schedule, so
     * tec_params_reload_if_needed() deliberately restores this value - see
     * s_loop_period_ms. Changing the period therefore requires a reboot. */
    s_loop_period_ms = s_p.control_period_ms;
    period_s = (float)s_loop_period_ms / 1000.0f;

    /* PID: output 0..100 %, I-term clamped for anti-windup, D-term filtered.
     * Gain convention (tec_pid.h): ki/kd are per-second rates, normalized
     * internally (ki*Ts, kd/Ts), so tuning stays valid if the task period
     * changes - only the period has to follow the QuarkTS schedule below. The
     * I-clamp and the D-filter factor are derived from the PHYSICAL-time
     * macros above for the same reason (period-independent dynamics).
     * The EPID is "heating semantics"; TecControl_Cb flips the sign of both
     * setpoint and measure ((x - SP) cooling error) of the cold side, so the
     * init seed MUST be in the same (negative) domain, otherwise the first
     * Update computes P/D against a +45 V..+45 C seed vs -sdm and slams the
     * output to 100 % even at zero error. Seed with the (negated) base
     * target: at init no measurement exists yet, and the first real
     * measurement replaces it next cycle.
     * tec_apply_pid_params() owns this call so the live-reload path applies
     * the gains through exactly the same expression - the two cannot drift.
     * The NaN seed makes the first call always take effect. */
    s_nv_gen  = NvParams_GetCacheGeneration();
    tec_apply_pid_params();

    s_alarm        = TEC_ALARM_NONE;
    s_tec_enabled  = 0u;
#if ( TEC_FAN_FAULT_ENABLE == 1 )
    s_fan_stall_ms = 0u;
    s_fan_grace_ms = 0u;     /* armed at the first-enable (fresh fan start) */
    s_fan_stalled  = 0u;
    s_fan_started  = 0u;
#endif
    s_dew_hold     = 0u;
    s_cold_fault   = 0u;
    tec_set_enabled( 0u );   /* held off until the SDM845 thermal precheck passes */

    /* Park the DAC on the zero-current end of the FB-injection window. In
     * the inverted topology DAC=0V is the FULL-cooling code (no injection
     * current -> VOUT at the divider set point), so leaving the power-up
     * 0 V there would rely on TEC_VEN alone as the single safety barrier.
     * With the sentinel set, this write always happens; from here on the
     * DAC sits at a code that commands zero TEC current even if the enable
     * line were ever asserted unexpectedly. */
    s_dac_percent  = 0xFFu;
    (void)tec_dac_set( 0u );

    (void)qOS_Add_Task( &s_task, TecControl_Cb, qLowest_Priority,
                        (qTime_t)period_s, qPeriodic,
                        qEnabled, NULL );
}

uint8_t TecControl_GetAlarm( void )
{
    return s_alarm;
}

void TecControl_ClearAlarm( void )
{
    s_alarm = TEC_ALARM_NONE;
}

uint8_t TecControl_GetDacPercent( void )
{
    return s_dac_percent;
}
