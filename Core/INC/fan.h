/**
 * @file fan.h
 * @brief Four-wire fan PWM control and tachometer.
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
#define FAN_TIMER_CLOCK_HZ        (8000000UL)
#endif

void Fan_Init(void);
void Fan_SetEnabled(uint8_t enabled);
void Fan_SetDutyPercent(uint8_t duty_percent);
uint8_t Fan_GetDutyPercent(void);
uint32_t Fan_GetTachPulses(void);
void Fan_ClearTachPulses(void);
uint32_t Fan_GetRPM(uint32_t sample_window_ms);
void Fan_Tach_IRQHandler(void);

#ifdef __cplusplus
}
#endif
#endif /* __FAN_H__ */