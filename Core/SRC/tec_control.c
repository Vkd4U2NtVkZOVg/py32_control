/**
 * @file tec_control.c
 * @brief TEC control task (QuarkTS periodic task).
 *
 * Reads all temperature/current sources, fuses them, runs PID + feedforward,
 * applies fault protection, and emits the TEC current command through the
 * MCP4725 DAC into the power-stage control window. The heatsink fan is
 * decoupled and follows the hot-side temperature. Non-blocking: a plain
 * periodic QuarkTS task (period from NVRAM, default 1 s) that runs to
 * completion and returns.
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
    int32_t target_base_millic;     /* base TEC target                          */
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
static volatile int32_t hot_mc, sdm_mc;   /* last hot/cold temperature readings */
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

/* Command the TEC current via the MCP4725: duty (0..100 %) is scaled into the
 * power-stage control window (0..dac_max_ctrl_mv). Writes only when the
 * value changed, to avoid pointless soft-I2C traffic every control cycle. */
static void tec_dac_set( uint8_t duty )
{
    uint16_t mv;

    if ( duty == s_dac_percent ) {
        return;
    }
    s_dac_percent = duty;
    mv = (uint16_t)( ( (uint32_t)duty * (uint32_t)s_p.dac_max_ctrl_mv ) / 100u );
    (void)MCP4725_SetVoltage_mV( mv );
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
    //int32_t hot_mc;
    int32_t env_mc;
    //int32_t sdm_mc;
    uint32_t cur_ma;
    int32_t composite_mc;
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

    /*--- 2. temperature fusion: hot*0.5 + sdm*0.3 + env*0.2 ----------------*/
    composite_mc = ( hot_mc * 5 + sdm_mc * 3 + env_mc * 2 ) / 10;

    /*--- 3. dynamic target: base + ambient compensation - SoC compensation --*/
    target_mc = s_p.target_base_millic;
    if ( env_mc > s_p.env_comp_temp_millic ) {
        target_mc += ( env_mc - s_p.env_comp_temp_millic ) / 2;  /* +0.5°C per °C above knee */
    }
    if ( sdm_mc > s_p.soc_tighten_temp_millic ) {
        target_mc -= s_p.soc_tighten_delta_millic;
    }

    /*--- 4. fault protection (highest priority first) ----------------------*/
    s_alarm = TEC_ALARM_NONE;

    if ( hot_mc > s_p.hot_danger_millic ) {
        /* danger: cut TEC, zero the DAC command, fan full speed */
        tec_set_enabled( 0u );
        tec_dac_set( 0u );
        Fan_SetDutyPercent( 100u );
        s_alarm |= ( TEC_ALARM_OVERTEMP | TEC_ALARM_HOT_WARNING );
        return;
    }

    /* Thermal precheck gate: the SDM845 boot state machine cools the enclosure
     * (fan full speed) until the AHT20 ambient is safe before starting the SoC
     * and TEC. While that is pending, hold the TEC off (DAC to 0 V) and do not
     * drive the fan (the precheck owns it), so we never fight the cooldown. */
    if ( Sdm845_Boot_IsThermalReady() == 0u ) {
        tec_set_enabled( 0u );
        tec_dac_set( 0u );
        s_alarm |= TEC_ALARM_THERMAL_WAIT;
        return;
    }

    if ( s_tec_enabled == 0u ) {
        tec_set_enabled( 1u );   /* precheck passed: normal operation, TEC on */
    }

    if ( hot_mc > s_p.hot_overtemp_millic ) {
        s_alarm |= TEC_ALARM_HOT_WARNING;
    }

    /*--- 5. PID main loop --------------------------------------------------*/
    /* EPID is "heating semantics" (error = SP - x, output increases when
     * temperature is below setpoint). Fan is a cooling actuator (output
     * increases when temperature is above setpoint), so we invert both
     * setpoint and measure to flip the effective error to (x - SP). */
    TecPid_SetSetpoint( &s_pid, -(float)target_mc );
    TecPid_Update( &s_pid, -(float)composite_mc );
    pid_out = (int32_t)TecPid_GetOutput( &s_pid );

    /*--- 6. feedforward from TEC current -----------------------------------*/
    feedforward = 0;
    if ( ( cur_ma <= (uint32_t)s_p.current_valid_max_ma ) &&
         ( cur_ma > (uint32_t)s_p.cur_feedforward_ma ) ) {
        feedforward = (int32_t)( cur_ma - (uint32_t)s_p.cur_feedforward_ma ) / 10;
    }
    if ( cur_ma > (uint32_t)s_p.cur_danger_ma ) {
        feedforward += 50;
        s_alarm |= TEC_ALARM_OVERCURRENT;
    }

    /*--- 7. combine, clamp, drive ------------------------------------------*/
    duty = pid_out + feedforward;
    if ( s_alarm & TEC_ALARM_HOT_WARNING ) {
        if ( duty < 100 ) {
            duty = 100;          /* overtemp: full speed */
        }
    }
    if ( duty > 100 ) {
        duty = 100;
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
    tec_dac_set( (uint8_t)duty );          /* TEC current command via MCP4725 */
}

/*============================================================================*/
/* Public API                                                                  */
/*============================================================================*/
void TecControl_Init( void )
{
    float period_s;

    tec_params_load( &s_p );

    /* PID: output 0..100 %, I-term clamped for anti-windup, D-term filtered. */
    (void)TecPid_Init( &s_pid,
                       s_p.pid_kp, s_p.pid_ki, s_p.pid_kd,
                       (float)s_p.target_base_millic,
                       0.0f, 100.0f,
                       -50.0f, 50.0f,
                       0.3f );

    s_alarm        = TEC_ALARM_NONE;
    s_tec_enabled  = 0u;
    s_dac_percent  = 0u;
    tec_set_enabled( 0u );   /* held off until the SDM845 thermal precheck passes */

    if ( s_p.control_period_ms < 1 ) {
        s_p.control_period_ms = 1000;   /* sanity: never schedule faster than 1 ms */
    }
    period_s = (float)s_p.control_period_ms / 1000.0f;

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