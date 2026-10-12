/**
 * @file tec_control.h
 * @brief TEC control task (QuarkTS periodic task).
 *
 * Cold-side temperature control + feedforward + fault protection.
 * The TEC current command (0..100 %) is emitted through the MCP4725 DAC into the
 * power-stage control window; the heatsink fan is decoupled and follows the
 * hot-side temperature instead:
 *   - Controlled variable: COLD side (sdm_mc) - PID feedback + dew-point guard
 *   - Hot side: protection only (danger shutdown, warning, fan ramp) - never
 *     enters the PID error
 *   - Ambient (env_mc): target compensation + common-mode feedforward
 *   - Dynamic target: base + env compensation - SoC compensation (cold-side)
 *   - PID main loop: TecPid_Update() with the cold-side temperature
 *   - Feedforward (optional, NV_TEC_FF_ENABLE): common-mode ambient-target
 *     term only - the former TEC-current term was removed (positive feedback,
 *     loop gain >1 inside the FF band); TEC current is protection-only now
 *   - Fault protection: overtemp full-speed/shutdown, overcurrent cutoff,
 *     fan fault alarm, dew-point hard cut + independent condensation floor,
 *     EN anti-hiccup deadband (deep light load drops TEC_VEN)
 *
 * All tunable parameters (PID gains, targets, thresholds, DAC window, fan ramp,
 * sensor scale) live in NVRAM (nv_params.h) and are snapshotted at
 * TecControl_Init() time - there are no tunable macros. Edit them over the I2C
 * parameter window (0x40.., see tres_i2c_slave.h) and save, or via the host
 * command SAVE_PARAMS.
 *
 * Dependencies (already implemented):
 *   - AHT20_Get_LastTempMilliCelsius() - environment temperature
 *   - Studio_ADC1_ReadHotMilliCelsius() - TEC hot-side temperature
 *   - Studio_ADC1_ReadCurrentMilliamp(&ma) - TEC current (reports sample
 *     validity: 0 = ADC timeout, the overcurrent guard must not run blind)
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

/* Alarm flags (bitmask) - informational, not tunable. */
#define TEC_ALARM_NONE              0x00
#define TEC_ALARM_OVERTEMP          0x01  /* hot-side > danger, TEC disabled   */
#define TEC_ALARM_HOT_WARNING       0x02  /* hot-side > overtemp, fan full-speed */
#define TEC_ALARM_OVERCURRENT       0x04  /* TEC current > danger, drive cut    */
#define TEC_ALARM_FAN_FAULT         0x08  /* fan RPM < min with duty > min     */
#define TEC_ALARM_THERMAL_WAIT      0x10  /* ambient precheck: TEC held off    */
#define TEC_ALARM_DEWPOINT          0x20  /* cold side < dew point + margin,
                                             TEC drive cut to avoid condensation */
#define TEC_ALARM_DAC_FAULT         0x40  /* MCP4725 write failed: commanded duty
                                             not applied (mis-actuation)        */
#define TEC_ALARM_SENSOR            0x80  /* temperature/current source invalid:
                                             NTC open/short, ADC timeout - TEC
                                             driven to a safe state               */

/**
 * @brief Initialize the TEC control task.
 *
 * Snapshots the NVRAM parameter table (nv_params.h), then creates a QuarkTS
 * periodic task (period from NVRAM, default 1 s, qLowest_Priority) that:
 *   1. Reads all temperature/current getters
 *   2. Computes the dynamic target temperature (cold side)
 *   3. Checks fault conditions (dew point, overtemp, overcurrent, thermal gate)
 *   4. Runs PID control on the cold side
 *   5. Adds feedforward compensation
 *   6. Outputs TEC duty to the MCP4725 DAC; fan duty from hot-side temperature
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