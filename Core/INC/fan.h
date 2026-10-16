/**
 * @file fan.h
 * @brief Four-wire fan PWM control and tachometer.
 *
 * The fan PWM is driven exclusively by the PY32 (TIM3 CH2); the hardware
 * no longer routes it to the SDM845 side (the FAN_CH_SW channel switch was
 * removed). Fan_SetDutyPercent() writes the TIM3 CH2 compare value directly
 * and always reaches the fan.
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