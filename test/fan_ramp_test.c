/* Standalone host test for the fan.c PWM soft ramp.
 * Built by run_fan_ramp_test.py: it strips the #include lines out of the
 * production fan.c and compiles it against the stubs below, so the exact
 * production Fan_SetDutyPercent() slewing logic is what gets asserted.
 */
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

/* ---- production constants (fan.h defaults, kept in sync by the runner) --- */
#define FAN_PWM_FREQUENCY_HZ      (25000UL)
#define FAN_TIMER_CLOCK_HZ        (32000000UL)
#define FAN_RAMP_MAX_PCT_PER_S    (30u)
#define FAN_PWM_PERIOD_TICKS      ((FAN_TIMER_CLOCK_HZ / FAN_PWM_FREQUENCY_HZ) - 1UL)

/* ---- hardware/peripheral stubs ------------------------------------------- */
#define TIM3  ((void *)0)
#define LL_EXTI_LINE_3 0x8u
#define LL_TIM_CHANNEL_CH2 2u
static uint32_t s_compare;                 /* TIM3 CH2 compare written by fan.c */
static uint32_t s_tick;                    /* fake 1 kHz qport_tick */
static int      s_exti3;
static uint32_t s_autoreload, s_cc_enabled, s_counter_enabled;
static uint32_t __get_PRIMASK(void) { return 0u; }
static void __disable_irq(void) {}
static void __set_PRIMASK(uint32_t m) { (void)m; }
static void LL_TIM_SetAutoReload(void *t, uint32_t v) { (void)t; s_autoreload = v; }
static void LL_TIM_OC_SetCompareCH2(void *t, uint32_t v) { (void)t; s_compare = v; }
static void LL_TIM_CC_EnableChannel(void *t, uint32_t c) { (void)t; s_cc_enabled = c; }
static void LL_TIM_EnableCounter(void *t) { (void)t; s_counter_enabled = 1u; }
static int  LL_EXTI_IsActiveFlag(uint32_t l) { return (s_exti3 & (int)l) != 0; }
static void LL_EXTI_ClearFlag(uint32_t l) { s_exti3 &= ~(int)l; }

/* ---- qport stub: the only real dependency fan.c needs -------------------- */
uint32_t QOS_Tick_Get(void) { return s_tick; }

#include "fan_impl.inc"    /* fan.c with #include lines stripped by the runner */

static uint32_t expected_compare(uint32_t duty)
{
    uint32_t c = ((FAN_PWM_PERIOD_TICKS + 1UL) * duty) / 100UL;
    if (c > FAN_PWM_PERIOD_TICKS) c = FAN_PWM_PERIOD_TICKS;
    return c;
}

int main(void)
{
    Fan_Init();
    assert(s_autoreload == FAN_PWM_PERIOD_TICKS);
    assert(s_compare == 0u);

    /* Enabled, first write (tick not yet advanced) must reach the target
     * immediately: boot full-speed behavior is preserved. */
    Fan_SetEnabled(1);
    s_tick = 0u;
    Fan_SetDutyPercent(100u);
    assert(s_compare == expected_compare(100u));

    /* Advance the tick and re-command 100 so the ramp timestamp is armed
     * from a non-zero tick (in the real firmware the scheduler has long
     * advanced the 1 kHz counter before the control task first writes). */
    s_tick = 1000u;
    Fan_SetDutyPercent(100u);
    assert(s_compare == expected_compare(100u));

    /* Ramp down 100 -> 30 at 30 %/s over 500 ms steps: 15 % per call. */
    s_tick = 1500u;  /* +500 ms -> max_step 15 */
    Fan_SetDutyPercent(30u);
    assert(s_compare == expected_compare(85u));
    s_tick += 500u; Fan_SetDutyPercent(30u);
    assert(s_compare == expected_compare(70u));
    s_tick += 500u; Fan_SetDutyPercent(30u);
    assert(s_compare == expected_compare(55u));
    s_tick += 500u; Fan_SetDutyPercent(30u);
    assert(s_compare == expected_compare(40u));
    s_tick += 500u; Fan_SetDutyPercent(30u);
    assert(s_compare == expected_compare(30u));   /* reached target */
    s_tick += 500u; Fan_SetDutyPercent(30u);
    assert(s_compare == expected_compare(30u));   /* stays at target */

    /* Small target change (30 -> 32) moves by the bounded step, not a jump. */
    s_tick += 500u; Fan_SetDutyPercent(32u);
    assert(s_compare == expected_compare(32u));   /* diff 2 < 15, done instantly */

    /* Long gap jumps straight to the target (a stalled writer must not make
     * the fan crawl; it restarts from where the fan physically is). */
    s_tick += 100000u;   /* +100 s */
    Fan_SetDutyPercent(60u);
    assert(s_compare == expected_compare(60u));

    /* Disabled fan: the compare is forced to 0 AND the slew state is
     * re-anchored to 0, so the pin really is at 0 % while disabled. A later
     * enable must therefore land its first request instead of stepping the
     * pin to the stale slewed value held before the disable. */
    s_tick += 100000u;                    /* long gap: park at 100 % */
    Fan_SetDutyPercent(100u);
    assert(s_compare == expected_compare(100u));
    Fan_SetEnabled(0);
    assert(s_compare == 0u);
    Fan_SetEnabled(1);
    s_tick += 500u;
    Fan_SetDutyPercent(30u);              /* lands at 85 % without the re-anchor */
    assert(s_compare == expected_compare(30u));

    /* From the re-anchored state the ordinary slew applies again. */
    s_tick += 500u;
    Fan_SetDutyPercent(0u);
    assert(s_compare == expected_compare(15u));   /* 30 - 15 % in one cycle */

    /* Protection writer: lands in the same call and re-anchors the slew
     * state, so the next normal write ramps from the forced duty (100) and
     * not from the value the ramp held before (15). */
    s_tick += 500u;
    Fan_SetDutyPercentImmediate(100u);
    assert(s_compare == expected_compare(100u));
    s_tick += 500u;
    Fan_SetDutyPercent(40u);
    assert(s_compare == expected_compare(85u));   /* 100 - 15 % */

    /* Clamp >100 input to 100; a long gap (>= 1 s with no writer) jumps
     * straight to the (clamped) target so a stalled writer cannot stall the
     * ramp. */
    s_tick += 1000u;
    Fan_SetDutyPercent(250u);
    assert(s_compare == expected_compare(100u));

    printf("fan ramp tests passed\n");
    return 0;
}
