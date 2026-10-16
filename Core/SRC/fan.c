/**
 * @file fan.c
 * @brief Four-wire fan PWM and tachometer implementation.
 */
#include "fan.h"

#define FAN_PWM_PERIOD_TICKS ((FAN_TIMER_CLOCK_HZ / FAN_PWM_FREQUENCY_HZ) - 1UL)

static volatile uint32_t s_tach_pulses;
static uint8_t s_duty_percent;
static uint8_t s_enabled;
static uint8_t s_source = (uint8_t)FAN_SOURCE_SDM845;

void Fan_Init(void)
{
    s_tach_pulses = 0U;
    s_duty_percent = 0U;
    s_enabled = 0U;
    Fan_SetSource( FAN_SOURCE_SDM845 );   /* default: hand the fan to the SoC */

    LL_TIM_SetAutoReload(TIM3, FAN_PWM_PERIOD_TICKS);
    LL_TIM_OC_SetCompareCH2(TIM3, 0U);
    LL_TIM_CC_EnableChannel(TIM3, LL_TIM_CHANNEL_CH2);
    LL_TIM_EnableCounter(TIM3);
}

void Fan_SetEnabled(uint8_t enabled)
{
    s_enabled = (enabled != 0U) ? 1U : 0U;
    Fan_SetSource( ( s_enabled != 0U ) ? FAN_SOURCE_PY32 : FAN_SOURCE_SDM845 );
    if ( s_enabled == 0U ) {
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

void Fan_SetSource(Fan_Source_t source)
{
    s_source = ( source == FAN_SOURCE_PY32 ) ? 1u : 0u;
    if ( s_source != 0u ) {
        LL_GPIO_ResetOutputPin( FAN_CH_SW_Port, FAN_CH_SW_Pin );  /* PY32 drives PWM */
    } else {
        LL_GPIO_SetOutputPin( FAN_CH_SW_Port, FAN_CH_SW_Pin ); /* SoC drives PWM */
    }
}

Fan_Source_t Fan_GetSource(void)
{
    return ( s_source != 0u ) ? FAN_SOURCE_PY32 : FAN_SOURCE_SDM845;
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

void Fan_Tach_IRQHandler(void)
{
    if (LL_EXTI_IsActiveFlag(LL_EXTI_LINE_3) != 0U) {
        LL_EXTI_ClearFlag(LL_EXTI_LINE_3);
        s_tach_pulses++;
    }
}
