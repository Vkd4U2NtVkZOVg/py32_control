/**
 * @file tec_control.c
 * @brief TEC control task (QuarkTS periodic task).
 *
 * Reads all temperature/current sources, fuses them, runs PID + feedforward,
 * applies fault protection, and emits the TEC current command through the
 * MCP4725 DAC into the power-stage control window. The heatsink fan is
 * decoupled and follows the hot-side temperature. Non-blocking: a plain
 * periodic QuarkTS task (1 s period) that runs to completion and returns.
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

/*============================================================================*/
/* Plausibility windows for sensor validity (milli-degC). NTC/AHT20 report    */
/* out-of-range as clamped extremes (see ntc3950.c), not sentinels, so a     */
/* reading outside the physical window is treated as invalid.                 */
/*============================================================================*/
#define TEC_TEMP_VALID_MIN_MILLIC   (-20000)   /* -20 °C */
#define TEC_TEMP_VALID_MAX_MILLIC   (120000)   /* 120 °C */

/* Current plausibility: 0..5000 mA is a sane TEC range. */
#define TEC_CURRENT_VALID_MAX_MA    (5000UL)

/* Fan fault needs the loop actually commanding airflow before it is meaningful. */
#define TEC_FAN_FAULT_MIN_DUTY      (30u)

/*============================================================================*/
/* Local state                                                                 */
/*============================================================================*/
static qTask_t   s_task;
static TecPid_t  s_pid;
static uint8_t   s_alarm;
static uint8_t   s_tec_enabled;
static uint8_t   s_dac_percent;      /* last TEC duty written to the MCP4725 */

/*============================================================================*/
/* Helpers                                                                     */
/*============================================================================*/
static int is_temp_valid( int32_t millic )
{
    return ( millic >= TEC_TEMP_VALID_MIN_MILLIC ) &&
           ( millic <= TEC_TEMP_VALID_MAX_MILLIC );
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
 * power-stage control window (0..TEC_DAC_MAX_CTRL_MV). Writes only when the
 * value changed, to avoid pointless soft-I2C traffic every 1 s cycle. */
static void tec_dac_set( uint8_t duty )
{
    uint16_t mv;

    if ( duty == s_dac_percent ) {
        return;
    }
    s_dac_percent = duty;
    mv = (uint16_t)( ( (uint32_t)duty * TEC_DAC_MAX_CTRL_MV ) / 100u );
    (void)MCP4725_SetVoltage_mV( mv, TEC_DAC_VREF_MV );
}

/* Heatsink fan is decoupled from the TEC PID output: base airflow while the
 * TEC is active, then a linear ramp over [TEC_FAN_TEMP_BASE, TEMP_FULL] on the
 * hot-side temperature, saturating at 100 %. */
static uint8_t tec_fan_duty_from_hot( int32_t hot_mc )
{
    int32_t duty = (int32_t)TEC_FAN_DUTY_BASE;

    if ( hot_mc > TEC_FAN_TEMP_BASE_MILLIC ) {
        duty = (int32_t)TEC_FAN_DUTY_BASE +
               ( ( hot_mc - TEC_FAN_TEMP_BASE_MILLIC ) *
                 ( 100 - (int32_t)TEC_FAN_DUTY_BASE ) ) /
               ( TEC_FAN_TEMP_FULL_MILLIC - TEC_FAN_TEMP_BASE_MILLIC );
    }
    if ( duty > 100 ) {
        duty = 100;
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
    sdm_mc = TRES_I2C_Slave_GetSdm845TempMilliC();
    cur_ma = Studio_ADC1_ReadCurrentMilliamp();

    if ( is_temp_valid( env_mc ) == 0 ) {
        env_mc = TEC_TARGET_BASE_MILLIC;   /* fall back to nominal ambient */
    }
    if ( is_temp_valid( sdm_mc ) == 0 ) {
        sdm_mc = TEC_TARGET_BASE_MILLIC;
    }

    /*--- 2. temperature fusion: hot*0.5 + sdm*0.3 + env*0.2 ----------------*/
    composite_mc = ( hot_mc * 5 + sdm_mc * 3 + env_mc * 2 ) / 10;

    /*--- 3. dynamic target: base + ambient compensation - SoC compensation --*/
    target_mc = TEC_TARGET_BASE_MILLIC;
    if ( env_mc > 35000 ) {
        target_mc += ( env_mc - 35000 ) / 2;   /* +0.5 °C per °C above 35 */
    }
    if ( sdm_mc > 70000 ) {
        target_mc -= 5000;                     /* tighten by 5 °C when SoC hot */
    }

    /*--- 4. fault protection (highest priority first) ----------------------*/
    s_alarm = TEC_ALARM_NONE;

    if ( hot_mc > TEC_HOT_DANGER_MILLIC ) {
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

    if ( hot_mc > TEC_HOT_OVERTEMP_MILLIC ) {
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
    if ( ( cur_ma <= TEC_CURRENT_VALID_MAX_MA ) &&
         ( cur_ma > TEC_CURRENT_FEEDFORWARD_MA ) ) {
        feedforward = (int32_t)( cur_ma - TEC_CURRENT_FEEDFORWARD_MA ) / 10;
    }
    if ( cur_ma > TEC_CURRENT_DANGER_MA ) {
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
    if ( ( fan_duty > TEC_FAN_FAULT_MIN_DUTY ) &&
         ( fan_rpm < FAN_MIN_RPM ) ) {
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
    /* PID: output 0..100 %, I-term clamped for anti-windup, D-term filtered. */
    (void)TecPid_Init( &s_pid,
                       TEC_PID_KP, TEC_PID_KI, TEC_PID_KD,
                       (float)TEC_TARGET_BASE_MILLIC,
                       0.0f, 100.0f,
                       -50.0f, 50.0f,
                       0.3f );

    s_alarm        = TEC_ALARM_NONE;
    s_tec_enabled  = 0u;
    s_dac_percent  = 0u;
    tec_set_enabled( 0u );   /* held off until the SDM845 thermal precheck passes */

    (void)qOS_Add_Task( &s_task, TecControl_Cb, qLowest_Priority,
                        (qTime_t)TEC_CONTROL_PERIOD_S, qPeriodic,
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
