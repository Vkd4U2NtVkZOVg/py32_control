/**
 * @file fan.h
 * @brief Four-wire fan PWM control and tachometer.
 *
 * Fan PWM ownership is a hardware channel on PA11 (FAN_CH_SW):
 *   LOW  -> the SDM845 side drives the fan PWM;
 *   HIGH -> the PY32 (TIM3 CH2) drives the fan PWM.
 * Use Fan_SetSource() to switch ownership; Fan_SetDutyPercent() is only
 * meaningful while the PY32 owns the channel.
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

/** Who drives the fan PWM through the PA11 channel switch. */
typedef enum {
    FAN_SOURCE_SDM845 = 0,  /**< PA11 low  -> fan PWM driven by the SDM845 side */
    FAN_SOURCE_PY32   = 1   /**< PA11 high -> fan PWM driven by the PY32 (TIM3) */
} Fan_Source_t;

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

/** @brief Select which side drives the fan PWM via the PA11 channel switch. */
void Fan_SetSource(Fan_Source_t source);
/** @brief Current fan PWM owner (see #Fan_Source_t). */
Fan_Source_t Fan_GetSource(void);

uint32_t Fan_GetTachPulses(void);
void Fan_ClearTachPulses(void);
void Fan_Tach_IRQHandler(void);

#ifdef __cplusplus
}
#endif
#endif /* __FAN_H__ */