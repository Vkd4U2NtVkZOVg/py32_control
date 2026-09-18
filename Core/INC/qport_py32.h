/*
 * qport_py32.h - QuarkTS runtime port interface for the PY32F030 project.
 */
#ifndef __QPORT_PY32_H__
#define __QPORT_PY32_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Register the kernel critical-section hooks and enable the 1 kHz
 *         SysTick interrupt that drives the time base.
 */
void QPort_Setup(void);

/**
 * @brief  Advance the millisecond time base. Called from SysTick_Handler().
 */
void QOS_Tick_Inc(void);

/**
 * @brief  Current millisecond tick value. Passed to qOS_Setup() as the tick
 *         source for the scheduler.
 * @retval monotonic milliseconds since boot.
 */
uint32_t QOS_Tick_Get(void);

#ifdef __cplusplus
}
#endif

#endif /* __QPORT_PY32_H__ */