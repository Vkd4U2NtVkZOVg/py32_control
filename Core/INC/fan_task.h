/**
 * @file fan_task.h
 * @brief Fan RPM measurement task (QuarkTS periodic task, non-blocking).
 *
 * A plain QuarkTS period task samples the fan tachometer once per cycle
 * (cycle == window == 2 s) and caches the latest RPM. The callback runs to
 * completion and returns, so it never blocks the scheduler; the ISR-side tach
 * counter keeps running between callbacks. Consumers read the cached value via
 * Fan_Get_LastRPM().
 *
 * This deliberately uses a plain periodic task, NOT a state machine: fan RPM is
 * a single uniform sampling window, unlike AHT20 which needs a state machine
 * to bridge multiple waits inside one measurement.
 */
#ifndef __FAN_TASK_H__
#define __FAN_TASK_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Register the RPM sampling task. Call after qOS_Setup and Fan_Init. */
void Fan_Task_Init(void);

/**
 * @brief Latest measured fan speed, updated every sample period.
 * @return RPM; 0 if the fan is off or not measured yet.
 */
uint32_t Fan_Get_LastRPM(void);

#ifdef __cplusplus
}
#endif

#endif /* __FAN_TASK_H__ */