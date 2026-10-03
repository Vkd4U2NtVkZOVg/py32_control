/**
 * @file tec_control.h
 * @brief TEC cooling fan control task (QuarkTS periodic task).
 *
 * Multi-source temperature fusion + PID control + feedforward + fault protection:
 *   - Temperature fusion: hot*0.5 + sdm845*0.3 + env*0.2
 *   - Dynamic target: 45°C + env compensation - SoC compensation
 *   - PID main loop: TecPid_Update() with fused temperature
 *   - Feedforward: TEC current > 2A adds duty proportional to current
 *   - Fault protection: overtemp full-speed/shutdown, overcurrent feedforward, fan fault alarm
 *
 * Dependencies (already implemented):
 *   - AHT20_Get_LastDewPointMilliCelsius() - environment temperature
 *   - Studio_ADC1_ReadHotMilliCelsius() - TEC hot-side temperature
 *   - Studio_ADC1_ReadCurrentMilliamp() - TEC current
 *   - Fan_Get_LastRPM() - fan speed
 *   - TRES_I2C_Slave_GetSdm845TempMilliC() - SDM845 SoC temperature
 *   - TecPid_* - PID controller
 *   - Fan_SetDutyPercent() - fan PWM control
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
 *   7. Outputs final duty to Fan_SetDutyPercent()
 *
 * Call after Fan_Task_Init() and AHT20_Task_Init().
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

#ifdef __cplusplus
}
#endif

#endif /* __TEC_CONTROL_H__ */
