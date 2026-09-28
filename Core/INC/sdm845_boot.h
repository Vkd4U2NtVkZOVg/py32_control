/**
 * @file sdm845_boot.h
 * @brief SDM845 boot/reset/strap state machine (QuarkTS).
 *
 * Implements the revised SDM845 boot state machine from WORKLOG:
 *
 *     [*] --> POWER_ON_WAIT --t_pwup--> PWR_PRESS --t_hold--> BOOT_CHECK
 *     BOOT_CHECK --booted--> BOOT_SUCCESS (done)
 *     BOOT_CHECK --15s 无响应--> BOOT_RESET --t_kill--> PMIC_SETTLE --t_settle--> PWR_PRESS (retry)
 *     BOOT_CHECK --15s 且 retry>=MAX--> STRAP_FAIL (halt)
 *
 * GPIO mapping (gpio.h): PWRKEY = MIX3_BOOTKEY (PB7), RST_KEY = MIX3_RST (PB8).
 * Boot success is decided by sdm845_detect (banner "Loader Build Info" matched
 * AND line idle for the required window).
 */
#ifndef __SDM845_BOOT_H__
#define __SDM845_BOOT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** State numbers (exposed for host STATUS / diagnostics). */
typedef enum {
    SDM845_POWER_ON_WAIT = 0,
    SDM845_PWR_PRESS,
    SDM845_BOOT_CHECK,
    SDM845_BOOT_SUCCESS,
    SDM845_BOOT_RESET,
    SDM845_PMIC_SETTLE,
    SDM845_STRAP_FAIL
} Sdm845_State_t;

/**
 * @brief Register and start the SDM845 boot state machine. Initial state is
 *        POWER_ON_WAIT, so cold boot runs automatically with no injected
 *        signal. Call after qOS_Setup, USART2_DMA_Start() and
 *        Sdm845_Detect_Init().
 */
void Sdm845_Boot_Init(void);

/** @return Non-zero once boot has been declared successful (latched). */
uint8_t Sdm845_Boot_IsDone(void);

/** @return Current state number (Sdm845_State_t). */
uint8_t Sdm845_Boot_GetState(void);

/** @return Number of BOOT_RESET retries so far. */
uint8_t Sdm845_Boot_GetRetries(void);

#ifdef __cplusplus
}
#endif

#endif /* __SDM845_BOOT_H__ */