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

void Fan_Init(void);
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