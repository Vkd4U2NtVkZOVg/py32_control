/**
 * @file fan.c
 * @brief Four-wire fan PWM and tachometer implementation.
 *
 * The compare value written to TIM3 CH2 is rate-limited so the fan never
 * receives an instantaneous duty step: Fan_SetDutyPercent() records the
 * requested target and slews the actual PWM compare toward it at
 * FAN_RAMP_MAX_PCT_PER_S %/s (time base: QOS_Tick_Get(), 1 kHz).
 * Fan_SetDutyPercentImmediate() writes the compare in the same call instead,
 * for the TEC protection paths (see fan.h).
 */
#include "fan.h"
#include "qport_py32.h"

#define FAN_PWM_PERIOD_TICKS ((FAN_TIMER_CLOCK_HZ / FAN_PWM_FREQUENCY_HZ) - 1UL)

/* Local prototype: Fan_Init() calls it before its definition, and the host
 * test harness compiles this file without fan.h. */
void fan_pwr_enable( uint8_t on );
static void fan_apply_pwm( uint8_t duty );

static volatile uint32_t s_tach_pulses;
static uint8_t s_duty_percent;
static uint8_t s_enabled;
static uint8_t  s_ramp_duty;       /* current slewed duty actually on the pin */
static uint32_t s_ramp_tick;       /* QOS_Tick_Get() of the last slew update */
static uint8_t  s_min_duty;        /* floor for every duty write (NVRAM base) */

void Fan_Init(void)
{
    s_tach_pulses = 0U;
    s_duty_percent = 0U;
    s_enabled = 0U;
    s_ramp_duty   = 0U;
    s_ramp_tick   = 0u;
    s_min_duty    = 0U;

    LL_TIM_SetAutoReload(TIM3, FAN_PWM_PERIOD_TICKS);
    LL_TIM_OC_SetCompareCH2(TIM3, 0U);
    LL_TIM_CC_EnableChannel(TIM3, LL_TIM_CHANNEL_CH2);
    LL_TIM_EnableCounter(TIM3);

    /* Power the fan rail - that is all the board needs. The fan's driver takes
     * any duty (down to 1 %) as soon as the rail is up, so there is no start-up
     * kick and no blocking delay here: the boot sequence is not delayed and
     * the pin simply stays at 0 % until something commands a duty.
     * The duty floor (Fan_SetMinDutyPercent, published from
     * NV_TEC_FAN_DUTY_BASE by TecControl_Init) is what lifts the pin off 0 %
     * for the first time, and keeps every later writer from going below the
     * base airflow. */
    fan_pwr_enable( 1u );                          /* rail on */
    s_enabled = 1U;                                /* rail up: duty API live */
}

void fan_pwr_enable(uint8_t on)
{
#if ( FAN_PWR_ACTIVE_HIGH != 0u )
    if ( on != 0u ) {
        LL_GPIO_SetOutputPin( FAN_PWR_EN_Port, FAN_PWR_EN_Pin );
    } else {
        LL_GPIO_ResetOutputPin( FAN_PWR_EN_Port, FAN_PWR_EN_Pin );
    }
#else
    if ( on != 0u ) {
        LL_GPIO_ResetOutputPin( FAN_PWR_EN_Port, FAN_PWR_EN_Pin );
    } else {
        LL_GPIO_SetOutputPin( FAN_PWR_EN_Port, FAN_PWR_EN_Pin );
    }
#endif
}
void Fan_SetEnabled(uint8_t enabled)
{
    s_enabled = (enabled != 0U) ? 1U : 0U;
    if ( s_enabled == 0U ) {
        LL_TIM_OC_SetCompareCH2(TIM3, 0U);
        /* Deliberate hard stop: the ONLY way to reach 0 % (nothing in the
         * firmware calls it). Re-anchor the slew state to 0 so a later enable
         * resumes from where the fan physically is. */
        s_ramp_duty  = 0U;
        s_ramp_tick  = 0U;
    }
}

/* Duty floor: the NVRAM base airflow. Clamped here so every writer inherits
 * it; if the pin is currently below a raised floor, lift it immediately
 * (raising airflow is always the safe direction). */
void Fan_SetMinDutyPercent(uint8_t pct)
{
    if ( pct > 100U ) {
        pct = 100U;
    }
    s_min_duty = pct;
    if ( s_enabled != 0U && s_ramp_duty < s_min_duty ) {
        s_ramp_duty = s_min_duty;
        fan_apply_pwm( s_ramp_duty );
    }
}

uint8_t Fan_GetMinDutyPercent(void)
{
    return s_min_duty;
}

static void fan_apply_pwm(uint8_t duty)
{
    uint32_t compare;

    if (duty > 100U) {
        duty = 100U;
    }
    if (duty < s_min_duty) {
        duty = s_min_duty;   /* never stop the fan through the duty path */
    }
    compare = ((FAN_PWM_PERIOD_TICKS + 1UL) * duty) / 100UL;
    if (compare > FAN_PWM_PERIOD_TICKS) {
        compare = FAN_PWM_PERIOD_TICKS;
    }
    LL_TIM_OC_SetCompareCH2(TIM3, compare);
}

/* Slews s_ramp_duty toward duty_percent by at most the rate limit scaled by
 * the time elapsed since the last call. The first call (tick delta >= ramp
 * period) reaches the target immediately so the fan never starts slower than
 * requested; a persistent writer that calls every 500 ms moves in bounded
 * steps and a one-off writer still completes its ramp on its own. */
void Fan_SetDutyPercent(uint8_t duty_percent)
{
    uint32_t now;
    uint32_t delta;
    uint32_t max_step;

    if (duty_percent > 100U) {
        duty_percent = 100U;
    }
    if (duty_percent < s_min_duty) {
        duty_percent = s_min_duty;   /* NVRAM base airflow is a hard floor */
    }
    s_duty_percent = duty_percent;

    now   = QOS_Tick_Get();
    delta = (uint32_t)( now - s_ramp_tick );

    if ( ( s_ramp_tick == 0u ) || ( delta >= 1000u ) ) {
        /* First call or long gap: jump straight to the target. A long gap
         * means no writer has been active (e.g. boot before the scheduler
         * tick is running, or a task stall), so ramping from a stale duty
         * would add an artificial delay. */
        s_ramp_duty = duty_percent;
    } else {
        max_step = (uint32_t)( ( (uint32_t)FAN_RAMP_MAX_PCT_PER_S *
                                 delta ) / 1000u );
        if ( max_step < 1u ) {
            max_step = 1u;   /* at least 1 % per call so a 500 ms writer moves */
        }
        if ( duty_percent > s_ramp_duty ) {
            if ( (uint32_t)( duty_percent - s_ramp_duty ) > max_step ) {
                s_ramp_duty = (uint8_t)( s_ramp_duty + max_step );
            } else {
                s_ramp_duty = duty_percent;
            }
        } else if ( duty_percent < s_ramp_duty ) {
            if ( (uint32_t)( s_ramp_duty - duty_percent ) > max_step ) {
                s_ramp_duty = (uint8_t)( s_ramp_duty - max_step );
            } else {
                s_ramp_duty = duty_percent;
            }
        }
    }
    s_ramp_tick = now;
    fan_apply_pwm( s_ramp_duty );
}

/* Immediate variant: lands the requested duty on the pin in this call and
 * re-anchors the slew state (s_ramp_duty/s_ramp_tick) to it, so a later
 * Fan_SetDutyPercent() slews from what is actually on the pin rather than
 * from the value the ramp had before. Used by the TEC protection paths; see
 * fan.h for why they must not wait out the slew. */
void Fan_SetDutyPercentImmediate(uint8_t duty_percent)
{
    if (duty_percent > 100U) {
        duty_percent = 100U;
    }
    if (duty_percent < s_min_duty) {
        duty_percent = s_min_duty;   /* NVRAM base airflow is a hard floor */
    }
    s_duty_percent = duty_percent;
    s_ramp_duty   = duty_percent;
    s_ramp_tick   = QOS_Tick_Get();
    fan_apply_pwm( duty_percent );
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

void Fan_Tach_IRQHandler(void)
{
    if (LL_EXTI_IsActiveFlag(LL_EXTI_LINE_3) != 0U) {
        LL_EXTI_ClearFlag(LL_EXTI_LINE_3);
        s_tach_pulses++;
    }
}