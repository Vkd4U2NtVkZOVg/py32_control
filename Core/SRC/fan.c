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

/* Local prototypes: Fan_Init() uses both before their definitions, and the
 * host test harness compiles this file without fan.h. The rail helper is
 * static - the rail is switched on once by the startup kick, never released. */
static void fan_pwr_enable( uint8_t on );
static void fan_apply_pwm( uint8_t duty );

static volatile uint32_t s_tach_pulses;
static uint8_t s_duty_percent;
static uint8_t s_enabled;
static uint8_t  s_ramp_duty;       /* current slewed duty actually on the pin */
static uint32_t s_ramp_tick;       /* QOS_Tick_Get() of the last slew update */
static uint8_t  s_min_duty;        /* floor for every duty write (NVRAM base) */
static uint8_t  s_kick_armed;      /* pin is at 100 % from the Fan_Init kick  */

void Fan_Init(void)
{
    s_tach_pulses = 0U;
    s_duty_percent = 0U;
    s_enabled = 0U;
    s_ramp_duty   = 0U;
    s_ramp_tick   = 0u;
    s_min_duty    = 0U;
    s_kick_armed  = 0u;

    LL_TIM_SetAutoReload(TIM3, FAN_PWM_PERIOD_TICKS);
    LL_TIM_OC_SetCompareCH2(TIM3, 0U);
    LL_TIM_CC_EnableChannel(TIM3, LL_TIM_CHANNEL_CH2);
    LL_TIM_EnableCounter(TIM3);

    /* Startup kick. The high-speed fan's driver IC needs >50 % PWM only at
     * RAIL POWER-UP - once unlatched the duty may go down to 1 % - so power
     * the rail and present 100 % before any task can command a low duty, then
     * block long enough for the driver to unlatch and the fan to spin up.
     * LL_mDelay() is safe here: Studio_RCC_Init already started the 1 ms
     * SysTick (LL_Init1msTick enables the timer, and LL_mDelay only polls its
     * COUNTFLAG - no interrupt required) and QuarkTS has not started yet, so
     * there is no task to starve. */
    fan_pwr_enable( 1u );                          /* rail on  */
    fan_apply_pwm( 100U );                         /* 100 % duty */
    LL_mDelay( FAN_STARTUP_KICK_MS );              /* spin up + unlatch */

    /* The pin is physically at 100 % now. Anchor the slew state on it and arm
     * the first writer so it GLIDES down instead of taking the "first call"
     * fast path (which would jump straight to a low duty): the TEC loop's
     * first command arrives with QOS_Tick_Get() still small, so without this
     * flag the anchor below would not even survive the 0-tick check. */
    s_enabled      = 1U;
    s_duty_percent = 100U;
    s_ramp_duty    = 100U;
    s_kick_armed   = 1U;
}

static void fan_pwr_enable(uint8_t on)
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
         * resumes from where the fan physically is, and drop the kick arm -
         * a stopped fan must spin up immediately, not glide. */
        s_ramp_duty  = 0U;
        s_ramp_tick  = 0U;
        s_kick_armed = 0U;
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
 * steps and a one-off writer still completes its ramp on its own. The one
 * exception is the first write after the Fan_Init() kick: the pin is known to
 * be at 100 %, so that call glides down instead of jumping. */
void Fan_SetDutyPercent(uint8_t duty_percent)
{
    uint32_t now;
    uint32_t delta;
    uint32_t max_step;
    uint8_t  glide;

    if (duty_percent > 100U) {
        duty_percent = 100U;
    }
    if (duty_percent < s_min_duty) {
        duty_percent = s_min_duty;   /* NVRAM base airflow is a hard floor */
    }
    s_duty_percent = duty_percent;

    now   = QOS_Tick_Get();
    delta = (uint32_t)( now - s_ramp_tick );

    /* First writer after the startup kick: the pin really is at 100 %, so
     * anchor the slew on that and pretend one nominal control period has
     * elapsed. Without this the "first call" fast path below would jump
     * straight from the 100 % kick to the (low) requested duty. */
    glide = s_kick_armed;
    if ( glide != 0u ) {
        s_kick_armed = 0U;
        s_ramp_duty  = 100U;
        s_ramp_tick  = now;
        delta        = FAN_RAMP_STEP_MS;
    }

    if ( ( glide == 0u ) && ( ( s_ramp_tick == 0u ) || ( delta >= 1000u ) ) ) {
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
    s_kick_armed  = 0U;              /* an explicit command supersedes the kick */
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