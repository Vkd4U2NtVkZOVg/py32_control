/**
 * @file fan_task.c
 * @brief Fan RPM measurement task (QuarkTS periodic task, non-blocking).
 *
 * A plain periodic QuarkTS task runs once per 2 s and computes the fan
 * speed from the tachometer pulses accumulated since the previous call:
 *
 *     rpm = delta_pulses * 60000 / (elapsed_ms * pulses_per_rev)
 *
 *   delta_pulses : tach counter delta over the natural 2 s window
 *                  (the window equals the task period);
 *   elapsed_ms   : actual elapsed time between the two callbacks (~2000 ms),
 *                  so any task-period drift is absorbed.
 *
 * The callback runs to completion and returns (no busy wait); the tach
 * counter keeps incrementing in the EXTI ISR between callbacks. This is a
 * plain periodic task on purpose: fan RPM is a single uniform sampling
 * window and needs no state machine (unlike the AHT20 multi-stage wait).
 */
#include "QuarkTS.h"
#include "fan_task.h"
#include "fan.h"
#include "qport_py32.h"
#include "adc.h"

/*============================================================================*/
/* Timing                                                                      */
/*============================================================================*/
#define FAN_SAMPLE_PERIOD_S   (2.0f)   /**< task period == RPM window (s)     */

/*============================================================================*/
/* Sampling state (touched only from the task callback)                        */
/*============================================================================*/
static qTask_t  s_fanTask;
static uint32_t s_prev_pulses;
static uint32_t s_prev_tick;
static uint32_t s_last_rpm;
/*============================================================================*/
/* Task callback: runs once per FAN_SAMPLE_PERIOD_S                            */
/*============================================================================*/
static void Fan_Task_Cb( qEvent_t e )
{
    uint32_t pulses      = Fan_GetTachPulses();
    uint32_t tick        = QOS_Tick_Get();
    uint32_t deltaPulses = pulses - s_prev_pulses;
    uint32_t elapsedMs   = tick - s_prev_tick;

    (void)e;

    if ( ( elapsedMs != 0u ) && ( FAN_TACH_PULSES_PER_REV != 0u ) ) {
        /* deltaPulses*60000 is bounded: at 10 kRPM / 2 pulses-per-rev the
         * 2 s window carries ~666 pulses -> ~40e6, well within uint64. */
        s_last_rpm = (uint32_t)( ( (uint64_t)deltaPulses * 60000ULL ) /
                                 ( (uint64_t)elapsedMs * FAN_TACH_PULSES_PER_REV ) );
    } else {
        s_last_rpm = 0u;
    }

    /* First invocation: prev_* start at 0, so it yields ~0 once. Fan off ->
     * deltaPulses stays 0 -> rpm 0 (a valid reading).                    */
    s_prev_pulses = pulses;
    s_prev_tick   = tick;
}

/*============================================================================*/
/* Public interface                                                            */
/*============================================================================*/
void Fan_Task_Init( void )
{
    s_prev_pulses = 0u;
    s_prev_tick   = 0u;
    s_last_rpm    = 0u;

    (void)qOS_Add_Task( &s_fanTask, Fan_Task_Cb, qLowest_Priority,
                        (qTime_t)FAN_SAMPLE_PERIOD_S, qPeriodic, qEnabled, NULL );
}

uint32_t Fan_Get_LastRPM( void )
{
    return s_last_rpm;
}