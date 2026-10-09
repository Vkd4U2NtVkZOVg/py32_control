/**
 * @file tec_control.h
 * @brief TEC control task (QuarkTS periodic task).
 *
 * Multi-source temperature fusion + PID control + feedforward + fault protection.
 * The TEC current command (0..100 %) is emitted through the MCP4725 DAC into the
 * power-stage control window; the heatsink fan is decoupled and follows the
 * hot-side temperature instead:
 *   - Temperature fusion: hot*0.5 + sdm845*0.3 + env*0.2
 *   - Dynamic target: 45°C + env compensation - SoC compensation
 *   - PID main loop: TecPid_Update() with fused temperature
 *   - Feedforward: TEC current > 2A adds duty proportional to current
 *   - Fault protection: overtemp full-speed/shutdown, overcurrent feedforward, fan fault alarm
 *
 * Dependencies (already implemented):
 *   - AHT20_Get_LastTempMilliCelsius() - environment temperature
 *   - Studio_ADC1_ReadHotMilliCelsius() - TEC hot-side temperature
 *   - Studio_ADC1_ReadCurrentMilliamp() - TEC current
 *   - Fan_Get_LastRPM() - fan speed
 *   - TRES_I2C_Slave_GetSdm845TempMilliC() - SDM845 SoC temperature
 *   - TecPid_* - PID controller
 *   - MCP4725_* - DAC actuator (TEC current command, soft-I2C)
 *   - Fan_SetDutyPercent() - fan PWM control (heatsink, decoupled)
 *   - TEC_VEN (PA12) - TEC enable pin
 */
#ifndef __TEC_CONTROL_H__
#define __TEC_CONTROL_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Alarm flags (bitmask) */
#define TEC_ALARM_NONE              0x00
#define TEC_ALARM_OVERTEMP          0x01  /* hot-side > 70°C, TEC disabled */
#define TEC_ALARM_HOT_WARNING       0x02  /* hot-side > 60°C, fan full-speed */
#define TEC_ALARM_OVERCURRENT       0x04  /* TEC current > 3A, feedforward +50% */
#define TEC_ALARM_FAN_FAULT         0x08  /* fan RPM < 500 with duty > 30% */
#define TEC_ALARM_THERMAL_WAIT      0x10  /* ambient precheck: TEC held off */

/* Control parameters (tunable) */
#define TEC_PID_KP                  2.0f
#define TEC_PID_KI                  0.1f
#define TEC_PID_KD                  0.5f
#define TEC_TARGET_BASE_MILLIC      45000   /* 45°C */
#define TEC_HOT_OVERTEMP_MILLIC     60000   /* 60°C */
#define TEC_HOT_DANGER_MILLIC       70000   /* 70°C */
#define TEC_CURRENT_FEEDFORWARD_MA  2000    /* 2A */
#define TEC_CURRENT_DANGER_MA       3000    /* 3A */
#define FAN_MIN_RPM                 500
#define TEC_CONTROL_PERIOD_S        1.0f

/* DAC actuator (MCP4725): maps the 0..100 % TEC command to the power-stage
 * control-window voltage. VREF = MCP4725 VDD; MAX_CTRL = top of the control
 * window the power stage accepts (TPS63802-VSET style input). */
#define TEC_DAC_VREF_MV             (3300u)
#define TEC_DAC_MAX_CTRL_MV         (1200u)

/* Heatsink fan is decoupled from the TEC PID output: linear ramp on the hot-side
 * temperature, base airflow while the TEC is active, full speed at TEMP_FULL. */
#define TEC_FAN_TEMP_BASE_MILLIC    (40000)  /* 40°C: fan starts ramping */
#define TEC_FAN_TEMP_FULL_MILLIC    (60000)  /* 60°C: fan full speed      */
#define TEC_FAN_DUTY_BASE           (30u)    /* base airflow while active */

/**
 * @brief Initialize the TEC control task.
 *
 * Creates a QuarkTS periodic task (1s period, qLowest_Priority) that:
 *   1. Reads all temperature/current getters
 *   2. Fuses temperatures (weighted average)
 *   3. Computes dynamic target temperature
 *   4. Runs PID control
 *   5. Adds feedforward compensation
 *   6. Checks fault conditions
 *   7. Outputs TEC duty to the MCP4725 DAC; fan duty from hot-side temperature
 *
 * Call after Fan_Task_Init(), AHT20_Task_Init() and MCP4725_Init().
 */
void TecControl_Init(void);

/**
 * @brief Get current alarm flags.
 * @return Bitmask of active alarms (TEC_ALARM_*).
 */
uint8_t TecControl_GetAlarm(void);

/**
 * @brief Clear alarm flags.
 */
void TecControl_ClearAlarm(void);

/**
 * @brief Current TEC current command (0..100 % of the DAC control window).
 * @return Last duty written to the MCP4725 this control cycle.
 */
uint8_t TecControl_GetDacPercent(void);

#ifdef __cplusplus
}
#endif

#endif /* __TEC_CONTROL_H__ */
