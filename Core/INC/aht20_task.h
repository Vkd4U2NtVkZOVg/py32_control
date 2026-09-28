/**
 * @file aht20_task.h
 * @brief AHT20 periodic dew-point measurement task (QuarkTS state machine).
 *
 * A QuarkTS state-machine task measures the AHT20 once every 10 s and caches
 * the latest dew point. The one-time power-up init runs in the initial
 * INIT state; all long delays (10 s cycle, 80 ms conversion wait, init
 * settle) are expressed as state-machine timeouts, so the task yields the CPU
 * instead of busy-waiting. Consumers read the cached value via
 * AHT20_Get_LastDewPointMilliCelsius().
 */
#ifndef __AHT20_TASK_H__
#define __AHT20_TASK_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Register the measurement task. Call after qOS_Setup and SoftI2C_Init. */
void AHT20_Task_Init(void);

/**
 * @brief Latest dew point produced by the periodic measurement task.
 * @return Dew point in milli-degrees Celsius, or INT32_MIN if no successful
 *         measurement has completed yet.
 */
int32_t AHT20_Get_LastDewPointMilliCelsius(void);

/**
 * @brief Result code of the most recent measurement step (AHT20_OK on success,
 *        otherwise one of the AHT20_ERR_* codes). Initially AHT20_ERR_NOT_READY.
 */
int AHT20_Get_LastStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* __AHT20_TASK_H__ */
