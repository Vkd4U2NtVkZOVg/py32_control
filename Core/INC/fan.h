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
 * time base. Every writer (TEC hot-side ramp, protection paths, boot code)
 * therefore reaches the fan through the same smooth ramp instead of an
 * audible step, while still reaching full speed quickly when needed.
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
 * fast enough for protection paths to reach full airflow promptly while
 * still eliminating audible step changes in the normal hot-side ramp. */
#ifndef FAN_RAMP_MAX_PCT_PER_S
#define FAN_RAMP_MAX_PCT_PER_S    (30u)
#endif

void Fan_Init(void);
void Fan_SetEnabled(uint8_t enabled);
void Fan_SetDutyPercent(uint8_t duty_percent);
uint8_t Fan_GetDutyPercent(void);

uint32_t Fan_GetTachPulses(void);
void Fan_ClearTachPulses(void);
void Fan_Tach_IRQHandler(void);

#ifdef __cplusplus
}
#endif
#endif /* __FAN_H__ */