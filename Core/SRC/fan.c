/**
 * @file fan.c
 * @brief Four-wire fan PWM and tachometer implementation.
 */
#include "fan.h"
#include "qport_py32.h"

#define FAN_PWM_PERIOD_TICKS ((FAN_TIMER_CLOCK_HZ / FAN_PWM_FREQUENCY_HZ) - 1UL)

static volatile uint32_t s_tach_pulses;
static uint8_t s_duty_percent;
static uint8_t s_enabled;

void Fan_Init(void)
{
    s_tach_pulses = 0U;
    s_duty_percent = 0U;
    s_enabled = 0U;

    LL_GPIO_ResetOutputPin(FAN_CH_SW_Port, FAN_CH_SW_Pin);
    LL_TIM_SetAutoReload(TIM3, FAN_PWM_PERIOD_TICKS);
    LL_TIM_OC_SetCompareCH2(TIM3, 0U);
    LL_TIM_CC_EnableChannel(TIM3, LL_TIM_CHANNEL_CH2);
    LL_TIM_EnableCounter(TIM3);
}

void Fan_SetEnabled(uint8_t enabled)
{
    s_enabled = (enabled != 0U) ? 1U : 0U;
    if (s_enabled != 0U) {
        LL_GPIO_SetOutputPin(FAN_CH_SW_Port, FAN_CH_SW_Pin);
    } else {
        LL_GPIO_ResetOutputPin(FAN_CH_SW_Port, FAN_CH_SW_Pin);
        LL_TIM_OC_SetCompareCH2(TIM3, 0U);
    }
}

void Fan_SetDutyPercent(uint8_t duty_percent)
{
    uint32_t compare;

    if (duty_percent > 100U) {
        duty_percent = 100U;
    }
    s_duty_percent = duty_percent;
    compare = ((FAN_PWM_PERIOD_TICKS + 1UL) * duty_percent) / 100UL;
    if (compare > FAN_PWM_PERIOD_TICKS) {
        compare = FAN_PWM_PERIOD_TICKS;
    }
    LL_TIM_OC_SetCompareCH2(TIM3, compare);
}

uint8_t Fan_GetDutyPercent(void)
{
    return s_duty_percent;
}

uint32_t Fan_GetTachPulses(void)
{
    return s_tach_pulses;
}

void Fan_ClearTachPulses(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_tach_pulses = 0U;
    __set_PRIMASK(primask);
}

uint32_t Fan_GetRPM(uint32_t sample_window_ms)
{
    uint32_t pulses;
    uint32_t start;

    if ((sample_window_ms == 0U) || (FAN_TACH_PULSES_PER_REV == 0U)) {
        return 0U;
    }
    Fan_ClearTachPulses();
    start = QOS_Tick_Get();
    while ((uint32_t)(QOS_Tick_Get() - start) < sample_window_ms) {
        /* wait while SysTick and the tach EXTI continue counting */
    }
    pulses = Fan_GetTachPulses();
    return (pulses * 60000UL) /
           (sample_window_ms * FAN_TACH_PULSES_PER_REV);
}

void Fan_Tach_IRQHandler(void)
{
    if (LL_EXTI_IsActiveFlag(LL_EXTI_LINE_3) != 0U) {
        LL_EXTI_ClearFlag(LL_EXTI_LINE_3);
        s_tach_pulses++;
    }
}
