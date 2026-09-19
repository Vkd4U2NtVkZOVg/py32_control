/**
  ******************************************************************************
  * @file    tres_seq.h
  * @brief   TRES (STM32H7) power/boot sequencer state machine (QuarkTS).
  *
  * Implements the state diagram documented in tres_sequencer_state_diagram.md:
  *
  *     OFF -> PWR_UP -> BOOT_SETUP -> BOOT_WAIT_NORMAL -> RUNNING
  *                              \-> BOOT_WAIT_DFU   -> DFU_ACTIVE
  *     PULSE_RESET (fast reset), SHUTDOWN (discharge guard) -> OFF,
  *     FAULT (latched, host-unlock only).
  *
  * Cold boot is autonomous: TRES_Seq_Init() injects CMD_PWR_ON(NORMAL) right
  * after the state machine is registered, so TRES boots from FLASH without any
  * host interaction. Afterwards the Linux host can only "change its mind"
  * through the command interface (tres_cmd.c): DFU, power-off, reset, fail
  * report.
  *
  * Feedback: TRES has no direct feedback path to this MCU (all four control
  * lines are outputs). SOM_BOOTED (PB4) is the Linux host's own boot-done
  * signal; because TRES is what starts the host, its rising edge is used as
  * the chained confirmation of a successful cold boot in BOOT_WAIT_NORMAL,
  * and its level feeds host_ready while RUNNING.
  *
  * Owned pins (see gpio.h):
  *     TRES_PWR_EN          PB6   main rail enable (1 = on)
  *     TRES_RST             PA8   NRST, push-pull (0 = held in reset)
  *     TRES_BOOT0           PB5   boot mode, latched by H7 on RST release
  *     TRES_VDD33USB_CTRL   PA15  TRES USB 3V3 rail (mandatory for DFU)
  ******************************************************************************
  */
#ifndef __TRES_SEQ_H__
#define __TRES_SEQ_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>
#include "main.h"
#include "gpio.h"

/* Exported constants --------------------------------------------------------*/
/* Sequencing timings in milliseconds (nominal placeholders: MEASURE THEM)    */
#ifndef TRES_T_RAIL_MS
#define TRES_T_RAIL_MS          50u     /**< main rail ramp-up (PWR_UP)      */
#endif
#ifndef TRES_T_SETUP_MS
#define TRES_T_SETUP_MS         5u      /**< BOOT0/VDD33USB settle (BOOT_SETUP) */
#endif
#ifndef TRES_T_APPBOOT_MS
#define TRES_T_APPBOOT_MS       2000u   /**< grace when SOM_BOOTED already high */
#endif
#ifndef TRES_T_HOSTBOOT_MS
#define TRES_T_HOSTBOOT_MS      60000u  /**< cold-boot supervision window       */
#endif
#ifndef TRES_T_ENUM_MS
#define TRES_T_ENUM_MS          3000u   /**< DFU USB enumeration window         */
#endif
#ifndef TRES_T_RST_MS
#define TRES_T_RST_MS           20u     /**< reset pulse width (PULSE_RESET)    */
#endif
#ifndef TRES_T_DISCHARGE_MS
#define TRES_T_DISCHARGE_MS     300u    /**< rail discharge guard (SHUTDOWN)    */
#endif
#ifndef TRES_N_MAX_RETRIES
#define TRES_N_MAX_RETRIES      3u      /**< consecutive failures -> FAULT      */
#endif
/* 1 = power the USB 3V3 rail also for normal boot (H7 app uses USB)          */
#ifndef TRES_NORMAL_VDD33USB_ON
#define TRES_NORMAL_VDD33USB_ON 1u
#endif
/* Optional: retry period while in FAULT (0 = disabled, host unlocks only)    */
#ifndef TRES_FAULT_RETRY_MS
#define TRES_FAULT_RETRY_MS     0u
#endif
/* Optional: power-cycle the whole chain when the host stays gone this long   */
/* after a SOM_BOOTED falling edge while RUNNING (0 = keep TRES running)      */
#ifndef TRES_T_HOSTLOST_MS
#define TRES_T_HOSTLOST_MS      90000u
#endif
#ifndef TRES_REBOOT_CHAIN_ON_HOST_LOST
#define TRES_REBOOT_CHAIN_ON_HOST_LOST 0u
#endif

/* Exported types ------------------------------------------------------------*/
/** Sequencer states, exposed as STATUS bit[3:0] to the host. */
typedef enum {
    TRES_ST_OFF = 0,
    TRES_ST_PWR_UP,
    TRES_ST_BOOT_SETUP,
    TRES_ST_BOOT_WAIT_NORMAL,
    TRES_ST_BOOT_WAIT_DFU,
    TRES_ST_RUNNING,
    TRES_ST_DFU_ACTIVE,
    TRES_ST_PULSE_RESET,
    TRES_ST_SHUTDOWN,
    TRES_ST_FAULT
} TRES_State_t;

/** Boot mode: normal FLASH boot or system-bootloader DFU. */
typedef enum {
    TRES_MODE_NORMAL = 0,
    TRES_MODE_DFU = 1
} TRES_Mode_t;

/* Exported functions --------------------------------------------------------*/
void TRES_Seq_Init(void);                 /**< register FSM task + autoboot  */

/* Request interface (command layer -> FSM signals). Non-blocking.            */
void TRES_Seq_RequestPowerOn(TRES_Mode_t mode);
void TRES_Seq_RequestPowerOff(void);
void TRES_Seq_RequestReset(void);
void TRES_Seq_RequestRebootNormal(void);
void TRES_Seq_RequestFailReport(void);

/* Status interface for the host register file.                               */
uint8_t TRES_Seq_GetState(void);          /**< TRES_State_t                  */
uint8_t TRES_Seq_GetRetries(void);
uint8_t TRES_Seq_IsBusy(void);            /**< 1 while sequencing SHUTDOWN   */
uint8_t TRES_Seq_HostReady(void);         /**< SOM_BOOTED level (host_ready) */
uint8_t TRES_Seq_FaultLatched(void);
/** bit0=PWR_EN,1=RST,2=BOOT0,3=VDD33USB,4=SOM_BOOTED input                   */
uint8_t TRES_Seq_GetPinsSnapshot(void);

#ifdef __cplusplus
}
#endif

#endif /* __TRES_SEQ_H__ */
