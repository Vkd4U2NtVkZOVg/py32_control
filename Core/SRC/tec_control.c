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
 * The dew-point guard is an independent hard protection layer on the cold
 * side (PID and guard share the same controlled variable, so they cannot
 * fight each other).
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
/* Configuration: snapshotted once from NVRAM at TecControl_Init().            */
/*============================================================================*/
typedef struct {
    float   pid_kp;                 /* PID gains (float, Q16 in NVRAM)         */
    float   pid_ki;
    float   pid_kd;
    int32_t target_base_millic;     /* base TEC target temp                     */
    int32_t hot_overtemp_millic;    /* fan full speed threshold                 */
    int32_t hot_danger_millic;      /* TEC off + fan 100 % threshold            */
    int32_t cur_feedforward_ma;     /* current feedforward knee                 */
    int32_t cur_danger_ma;          /* overcurrent alarm + FF boost threshold   */
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
    int32_t dac_min_ctrl_mv;        /* FB-injection window bottom (VFBTH point) */
    int32_t ramp_step_percent;      /* max duty change per cycle (soft start)   */
    int32_t dewpoint_margin_millic; /* target floor = dew point + this          */
    float   ff_gain;                /* COMMON-MODE feedforward %/°C (Q16 NVRAM) */
} Tec_Params_t;

static void tec_params_load( Tec_Params_t *p )
{
    p->pid_kp                = (float)NvParams_Get( NV_TEC_PID_KP_Q16 ) / NVM_Q16_SCALE;
    p->pid_ki                = (float)NvParams_Get( NV_TEC_PID_KI_Q16 ) / NVM_Q16_SCALE;
    p->pid_kd                = (float)NvParams_Get( NV_TEC_PID_KD_Q16 ) / NVM_Q16_SCALE;
    p->target_base_millic    = (int32_t)NvParams_Get( NV_TEC_TARGET_BASE_MILLIC );
    p->hot_overtemp_millic   = (int32_t)NvParams_Get( NV_TEC_HOT_OVERTEMP_MILLIC );
    p->hot_danger_millic     = (int32_t)NvParams_Get( NV_TEC_HOT_DANGER_MILLIC );
    p->cur_feedforward_ma    = (int32_t)NvParams_Get( NV_TEC_CURRENT_FEEDFORWARD_MA );
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
    p->dac_min_ctrl_mv        = (int32_t)NvParams_Get( NV_TEC_DAC_MIN_CTRL_MV );
    p->ramp_step_percent      = (int32_t)NvParams_Get( NV_TEC_RAMP_STEP_PERCENT );
    p->dewpoint_margin_millic = (int32_t)NvParams_Get( NV_TEC_DEWPOINT_MARGIN_MILLIC );
    p->ff_gain                = (float)NvParams_Get( NV_TEC_FF_GAIN_Q16 ) / NVM_Q16_SCALE;

    /* Sanity-check the DAC FB-injection window: tec_dac_set() computes
     * span = dac_max - dac_min and multiplies duty*span, so a corrupt
     * window (host-written NVRAM or a FLASH bit flip) where min >= max
     * would wrap span to ~2^32 and make the DAC output an arbitrary
     * voltage. Fall back to the factory defaults on any violation. */
    if ( ( p->dac_min_ctrl_mv < 0 ) || ( p->dac_max_ctrl_mv < 0 ) ||
         ( p->dac_min_ctrl_mv > (int32_t)MCP4725_VDD_MV ) ||
         ( p->dac_max_ctrl_mv > (int32_t)MCP4725_VDD_MV ) ||
         ( p->dac_min_ctrl_mv >= p->dac_max_ctrl_mv ) ) {
        p->dac_min_ctrl_mv = 0;
        p->dac_max_ctrl_mv = 1200;   /* NVRAM factory default (NV_TEC_DAC_MAX_CTRL_MV) */
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
        p->fan_temp_base_millic = 40000u;
        p->fan_temp_full_millic = 60000u;
    }
    if ( p->hot_overtemp_millic >= p->hot_danger_millic ) {
        p->hot_overtemp_millic = 60000u;
        p->hot_danger_millic   = 70000u;
    }
    if ( p->cur_feedforward_ma >= p->cur_danger_ma ) {
        p->cur_feedforward_ma = 2000u;
        p->cur_danger_ma      = 3000u;
    }
    if ( ( p->pid_kp <= 0.0f ) || ( p->pid_ki <= 0.0f ) || ( p->pid_kd < 0.0f ) ) {
        p->pid_kp = 2.0f;
        p->pid_ki = 0.1f;
        p->pid_kd = 0.5f;
    }
    if ( ( p->ramp_step_percent <= 0 ) || ( p->ramp_step_percent > 100 ) ) {
        p->ramp_step_percent = 5u;
    }
    if ( ( p->fan_duty_base < 0 ) || ( p->fan_duty_base > 100 ) ) {
        p->fan_duty_base = 30u;
    }
    if ( p->dewpoint_margin_millic < 0 ) {
        p->dewpoint_margin_millic = 2000u;
    }
    if ( p->temp_valid_min_millic >= p->temp_valid_max_millic ) {
        p->temp_valid_min_millic = (int32_t)( -20000 );
        p->temp_valid_max_millic = 120000u;
    }
    if ( p->current_valid_max_ma <= 0 ) {
        p->current_valid_max_ma = 5000u;
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
static uint8_t     s_dac_percent;      /* last TEC duty written to the MCP4725 */

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
 * NOTE on startup: we do NOT start from duty ~0 (VOUT ~ 0). Sweeping the full
 * 0..6 V range would drag the D-CAP2 control across the low-duty region where
 * its loop is less well-behaved. Instead the startup point sits NEAR the
 * operating point (DAC_STARTUP_PCT, default 60 % of full cooling) and the
 * large 0 -> anchor jump is handled by the TPS565201's own 1 ms internal soft
 * start; the ramp then only trims a small range toward the target.
 *
 * Failure handling: the DAC write status is returned; s_dac_percent is only
 * advanced on SUCCESS, so a failed write never pretends the value was applied
 * (a stale DAC output while the control loop thinks it moved the TEC voltage
 * would be a silent mis-actuation). Callers that gate TEC enable on this must
 * check the return. */
static int tec_dac_set( uint8_t duty )
{
    uint16_t mv;
    uint32_t span;
    MCP4725_Status_t st;

    if ( duty == s_dac_percent ) {
        return MCP4725_OK;
    }
    span = (uint32_t)( s_p.dac_max_ctrl_mv - s_p.dac_min_ctrl_mv );
    mv = (uint16_t)( (uint32_t)s_p.dac_max_ctrl_mv -
                     ( ( (uint32_t)duty * span ) / 100u ) );
    st = MCP4725_SetVoltage_mV( mv );
    if ( st != MCP4725_OK ) {
        return (int)st;   /* failed: keep s_dac_percent at the last-good value */
    }
    s_dac_percent = duty;
    return MCP4725_OK;
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
    int32_t hot_mc;
    int32_t env_mc;
    int32_t sdm_mc;
    uint32_t cur_ma;
    int32_t target_mc;
    int32_t pid_out;
    int32_t feedforward;
    int32_t duty;
    uint8_t fan_duty;
    uint32_t fan_rpm;

    (void)e;

    /*--- 1. read all sources ------------------------------------------------*/
    hot_mc = Studio_ADC1_ReadHotMilliCelsius();
    env_mc = AHT20_Get_LastTempMilliCelsius();   /* ambient, not dew point */
    sdm_mc = Studio_ADC1_ReadColdMilliCelsius();
    cur_ma = Studio_ADC1_ReadCurrentMilliamp();

    if ( is_temp_valid( env_mc ) == 0 ) {
        env_mc = s_p.target_base_millic;   /* fall back to nominal ambient */
    }
    if ( is_temp_valid( sdm_mc ) == 0 ) {
        sdm_mc = s_p.target_base_millic;
    }

    /*--- 1b. hot-side sensor failure is FAIL-SAFE, not fall-back -----------*/
    /* The cold side and ambient fall back to the target on an invalid
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

    /*--- 2. target temperature --------------------------------------------*/
    target_mc = s_p.target_base_millic;
    if ( env_mc > s_p.env_comp_temp_millic ) {
        target_mc += ( env_mc - s_p.env_comp_temp_millic ) / 2;  /* +0.5°C per °C above knee */
    }
    if ( sdm_mc > s_p.soc_tighten_temp_millic ) {
        target_mc -= s_p.soc_tighten_delta_millic;
    }

    /*--- 4. fault protection (highest priority first) ----------------------*/
    s_alarm = TEC_ALARM_NONE;

    /* Condensation protection - the controlled variable IS the cold side
     * (sdm_mc), and this guard is the independent hard protection layer on
     * top of the PID. If the cold side reaches (dew point + margin), cut the
     * TEC drive so it warms back above the condensation floor. */
    {
        int32_t dew_mc = AHT20_Get_LastDewPointMilliCelsius();

        if ( dew_mc != INT32_MIN ) {
            int32_t floor_mc = dew_mc + s_p.dewpoint_margin_millic;

            if ( sdm_mc < floor_mc ) {
                tec_set_enabled( 0u );
                (void)tec_dac_set( 0u );
                Fan_SetDutyPercent( 100u );
                s_alarm |= TEC_ALARM_DEWPOINT;
                return;
            }
        }
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

    if ( s_tec_enabled == 0u ) {
        /* First enable: settle the FB node at the startup point BEFORE
         * asserting TEC_VEN, then let the ramp take over.
         *
         * The startup point is a PERCENT of full cooling (DAC_STARTUP_PCT,
         * default 60), chosen near the operating point, not at the 0-V end.
         * Sweeping the whole 0..full range would drag the D-CAP2 loop through
         * the low duty / low output region; instead the TPS565201's own 1 ms
         * internal soft start builds the output from 0 V up to wherever the
         * FB pin settles at this DAC %, and the per-cycle ramp below only
         * trims the small remaining range toward the target. On asserting EN
         * with the anchor set, FB is already above the UVP threshold
         * (65 % of 0.76 V), so no hiccup. */
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
            s_dac_percent = 0u;   /* force a write */
            dac_st = tec_dac_set( start_duty );   /* settle FB before enabling */
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
        tec_set_enabled( 1u );   /* precheck passed: normal operation, TEC on */

        /* Take ownership of the fan PWM channel. Fan_Init() defaults the
         * FAN_CH_SW source switch to the SDM845 (the SoC drives the PWM), so
         * writing TIM3 CH2 duty alone is a no-op until we flip ownership to
         * the PY32. Without this the TEC loop's fan ramps (full speed on
         * overtemp, hot-side following) never reach the fan. */
        Fan_SetEnabled( 1u );
    }

    if ( hot_mc > s_p.hot_overtemp_millic ) {
        s_alarm |= TEC_ALARM_HOT_WARNING;
    }

    /*--- 5. PID main loop --------------------------------------------------*/
    /* The controlled variable is the COLD side (sdm_mc); the hot side and
     * ambient never enter the PID error. EPID is "heating semantics"
     * (error = SP - x, output increases when temperature is below setpoint).
     * Fan is a cooling actuator (output increases when temperature is above
     * setpoint), so we invert both setpoint and measure to flip the effective
     * error to (x - SP). */
    TecPid_SetSetpoint( &s_pid, -(float)target_mc );
    TecPid_Update( &s_pid, -(float)sdm_mc );
    pid_out = (int32_t)TecPid_GetOutput( &s_pid );

    /*--- 5b. COMMON-MODE feedforward (TI AFEx39xx "COMMON-MODE" idea):
     * the nominal actuator effort to hold a given target is dominated by the
     * ambient-to-target temperature difference (the TEC must pump heat against
     * the ambient gradient). Instead of letting the integrator crawl there from
     * zero every time, start from an estimated base duty and let the PI loop
     * only correct the residual error. Gain is % duty per °C of (env - target).
     * NOTE: gradient_mc is in milli-degC, so it must be converted to degC
     * before multiplying by ff_gain (% per degC) - otherwise a 10 degC
     * gradient (10000 mdegC) x 1.0 saturates the feedforward to 100 % and
     * swamps the PID. */
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

    /*--- 6. feedforward from TEC current (on top of COMMON-MODE) ----------*/
    if ( ( cur_ma <= (uint32_t)s_p.current_valid_max_ma ) &&
         ( cur_ma > (uint32_t)s_p.cur_feedforward_ma ) ) {
        feedforward += (int32_t)( cur_ma - (uint32_t)s_p.cur_feedforward_ma ) / 10;
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

    /*--- 7. combine, clamp, soft-start ramp --------------------------------*/
    duty = pid_out + feedforward;
    if ( s_alarm & TEC_ALARM_HOT_WARNING ) {
        /* Hot-side overtemperature warning: DO NOT push more cooling. A TEC
         * pumps waste heat into its hot side, so forcing duty=100 here would
         * add more heat to an already-too-hot heatsink - same positive-
         * feedback class as the overcurrent bug. Drop the TEC drive instead
         * and let the full-speed fan (section 8: hot >= fan_temp_full ->
         * 100 %) shed the heat; the soft-start ramp limits how fast the duty
         * falls, avoiding a hard step on the power stage. */
        if ( duty > 0 ) {
            TecPid_Reset( &s_pid, -(float)sdm_mc );
        }
        duty = 0;
    }
    if ( duty > 100 ) {
        duty = 100;
    }
    if ( duty < 0 ) {
        duty = 0;
    }

    /* Soft-start: never step the TEC duty by more than ramp_step_percent per
     * cycle. The DAC startup point (NV_TEC_DAC_STARTUP_PCT, % of full scale) is
     * where the TEC control starts from after enable - on a pure buck
     * (TPS565201) the FB regulation keeps the TEC voltage pinned until the DAC
     * crosses the power-stage set point, so starting near the operating point
     * and ramping limits the inrush and avoids slamming the TEC (TI SNVAA70
     * §3.3 "start close to Vin and regulate slowly"). */
    if ( duty > ( (int32_t)s_dac_percent + s_p.ramp_step_percent ) ) {
        duty = (int32_t)s_dac_percent + s_p.ramp_step_percent;
    }
    if ( duty < ( (int32_t)s_dac_percent - s_p.ramp_step_percent ) ) {
        duty = (int32_t)s_dac_percent - s_p.ramp_step_percent;
    }
    if ( duty < 0 ) {
        duty = 0;
    }

    /*--- 8. fan fault detection (only when commanding real airflow) --------*/
    fan_duty = tec_fan_duty_from_hot( hot_mc );
    fan_rpm  = Fan_Get_LastRPM();
    if ( ( fan_duty > (uint8_t)s_p.fan_fault_min_duty ) &&
         ( fan_rpm < (uint32_t)s_p.fan_min_rpm ) ) {
        s_alarm |= TEC_ALARM_FAN_FAULT;
    }

    Fan_SetDutyPercent( fan_duty );        /* heatsink fan: follows hot side */
    if ( tec_dac_set( (uint8_t)duty ) != MCP4725_OK ) {
        /* DAC write failed mid-run: the TEC voltage is NOT what the loop
         * thinks. Flag it so the host can see the mis-actuation; the next
         * cycle retries the write (s_dac_percent was kept at last-good). */
        s_alarm |= TEC_ALARM_DAC_FAULT;
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
    period_s = (float)s_p.control_period_ms / 1000.0f;

    /* PID: output 0..100 %, I-term clamped for anti-windup, D-term filtered.
     * Gain convention (tec_pid.h): ki/kd are per-second rates, scaled
     * internally by period_s, so tuning stays valid if the task period
     * changes - only period_s has to follow the QuarkTS schedule below.
     * The EPID is "heating semantics"; TecControl_Cb flips the sign of both
     * setpoint and measure ((x - SP) cooling error) of the cold side, so the
     * init seed MUST be in the same (negative) domain, otherwise the first
     * Update computes P/D against a +45 V..+45 C seed vs -sdm and slams the
     * output to 100 % even at zero error. Seed with the (negated) base
     * target: at init no measurement exists yet, and the first real
     * measurement replaces it next cycle. */
    (void)TecPid_Init( &s_pid,
                       s_p.pid_kp, s_p.pid_ki, s_p.pid_kd,
                       period_s,
                       -(float)s_p.target_base_millic,
                       0.0f, 100.0f,
                       -50.0f, 50.0f,
                       0.3f );

    s_alarm        = TEC_ALARM_NONE;
    s_tec_enabled  = 0u;
    s_dac_percent  = 0u;
    tec_set_enabled( 0u );   /* held off until the SDM845 thermal precheck passes */

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