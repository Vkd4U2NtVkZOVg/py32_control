/**
 * @file fan.h
 * @brief Four-wire fan PWM control and tachometer.
 *
 * The fan PWM is driven exclusively by the PY32 (TIM3 CH2); the hardware
 * no longer routes it to the SDM845 side (the FAN_CH_SW channel switch was
 * removed). Fan_SetDutyPercent() writes the TIM3 CH2 compare value directly
 * and always reaches the fan.
 *
 * Fan_SetDutyPercent() applies a rate-limited soft ramp on top of the raw
 * compare write: a requested duty is slewed toward the target at no more
 * than FAN_RAMP_MAX_PCT_PER_S per second, using the 1 kHz QOS_Tick_Get()
 * time base. Ordinary writers (the TEC hot-side ramp, boot code) therefore
 * reach the fan through the same smooth ramp instead of an audible step.
 *
 * Fan_SetDutyPercentImmediate() bypasses the slew and lands the requested
 * duty in this call. It is reserved for the TEC protection paths (overtemp,
 * invalid sensor, overcurrent, stalled fan): they re-command every control
 * period, so the >= 1 s "writer gap" fast path of the soft ramp never
 * applies to them and the protection demand would crawl up at 30 %/s
 * (30 -> 100 % takes ~2.5 s) exactly when full airflow is the point.
 *
 * Fan power rail: the high-speed fan's driver IC requires >50 % PWM on its
 * first application, otherwise it stays latched-off with no response.
 * Fan_Init() therefore powers the rail (FAN_PWR_EN, active-high) AND writes
 * 100 % duty, then blocks for FAN_STARTUP_KICK_MS so the fan spins up and
 * the driver unlatches BEFORE the TEC task first commands a (possibly low)
 * duty. QuarkTS has not started yet, so the blocking delay is safe here.
 */
#ifndef __FAN_H__
#define __FAN_H__

#include <stdint.h>
#include "main.h"
#include "gpio.h"
#include "tim.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FAN_TACH_PULSES_PER_REV
#define FAN_TACH_PULSES_PER_REV  (2U)
#endif
#ifndef FAN_PWM_FREQUENCY_HZ
#define FAN_PWM_FREQUENCY_HZ      (25000UL)
#endif
#ifndef FAN_TIMER_CLOCK_HZ
#define FAN_TIMER_CLOCK_HZ        (32000000UL)
#endif
/* Soft-ramp rate limit: the fan duty slews toward the requested value at no
 * more than this many percent per second. 30 %/s moves 30..100 % in ~2.3 s,
 * which is smooth enough to remove audible steps in the normal hot-side ramp.
 * The protection paths do NOT rely on it - they use the immediate writer. */
#ifndef FAN_RAMP_MAX_PCT_PER_S
#define FAN_RAMP_MAX_PCT_PER_S    (30u)
#endif
/* Startup kick: how long Fan_Init() holds the fan at 100 % duty after
 * powering the rail, to unlatch the high-speed fan driver. Tune on the bench:
 * too short = the driver stays latched off (no response); longer than needed
 * just means a louder spin-up. */
#ifndef FAN_STARTUP_KICK_MS
#define FAN_STARTUP_KICK_MS       (1000u)
#endif
/* Fan power-enable polarity. The current board drives a high-side switch:
 * FAN_PWR_EN high = rail on. On an inverting board set this to 0. */
#ifndef FAN_PWR_ACTIVE_HIGH
#define FAN_PWR_ACTIVE_HIGH       (1u)
#endif

void Fan_Init(void);
void fan_pwr_enable(uint8_t on);
void Fan_SetEnabled(uint8_t enabled);
void Fan_SetDutyPercent(uint8_t duty_percent);
void Fan_SetDutyPercentImmediate(uint8_t duty_percent);
uint8_t Fan_GetDutyPercent(void);

uint32_t Fan_GetTachPulses(void);
void Fan_ClearTachPulses(void);
void Fan_Tach_IRQHandler(void);

#ifdef __cplusplus
}
#endif
#endif /* __FAN_H__ */