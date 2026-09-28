/**
  ******************************************************************************
  * @file    tres_seq.h
  * @brief   TRES (STM32H7) boot/reset/DFU sequencer state machine (QuarkTS).
  *
  * Implements the final state diagram (tres_sequencer_state_diagram.md):
  *
  *     PWR_UP -> BOOT_WAIT_NORMAL -> RUNNING
  *                                    |  CMD_RESET
*     RUNNING --CMD_RESET--> PULSE_RESET_NORMAL --BOOT0=0--> BOOT_WAIT_NORMAL
 *     RUNNING --CMD_ENTER_DFU--> PULSE_RESET_DFU --BOOT0=1--> BOOT_WAIT_DFU
 *                          -> DFU_ACTIVE --CMD_RESET--> PULSE_RESET_NORMAL (exit DFU)
  *
  * Cold boot is unconditional: the initial state IS PWR_UP, so TRES always
  * boots NORMAL (BOOT0=0) with no host interaction. There is no OFF / power
  * management and no soft power-off: the machine is always powered once it is
  * running. DFU is entered only at runtime, from RUNNING, by a reset pulse
  * with BOOT0 pulled high (no full power cycle); VDD33USB is powered on only
  * for that DFU entry and switched off again on exit.
  *
  * Feedback: TRES has no direct feedback path to this MCU. SOM_BOOTED (PB4)
  * is the Linux host's own boot-done signal; because TRES starts the host, its
  * rising edge confirms the whole boot chain in BOOT_WAIT_NORMAL, and its
  * level feeds host_ready while RUNNING. If TRES never brings the host up,
  * BOOT_WAIT_NORMAL waits indefinitely (no supervision timeout).
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
#ifndef TRES_T_APPBOOT_MS
#define TRES_T_APPBOOT_MS       2000u   /**< grace when SOM_BOOTED already high */
#endif
#ifndef TRES_T_RST_MS
#define TRES_T_RST_MS           20u     /**< reset pulse width (PULSE_RESET)  */
#endif
#ifndef TRES_T_ENUM_MS
#define TRES_T_ENUM_MS          3000u   /**< DFU USB enumeration window       */
#endif

/* Exported types ------------------------------------------------------------*/
/** Sequencer states, exposed as STATUS bit[3:0] to the host. */
typedef enum {
    TRES_PWR_UP = 0,
    TRES_BOOT_WAIT_NORMAL,
    TRES_RUNNING,
    TRES_PULSE_RESET_NORMAL,
    TRES_PULSE_RESET_DFU,
    TRES_BOOT_WAIT_DFU,
    TRES_DFU_ACTIVE
} TRES_State_t;

/* Exported functions --------------------------------------------------------*/
void TRES_Seq_Init(void);                 /**< register FSM task (initial= PWR_UP) */

/* Request interface (command layer -> FSM signals). Non-blocking.            */
void TRES_Seq_RequestReset(void);         /**< reset pulse, boot NORMAL (BOOT0=0)  */
void TRES_Seq_RequestEnterDfu(void);      /**< reset pulse, boot DFU (BOOT0=1, USB rail up) */

/* Status interface for the host register file.                               */
uint8_t TRES_Seq_GetState(void);          /**< TRES_State_t                  */
uint8_t TRES_Seq_HostReady(void);         /**< SOM_BOOTED level (host_ready) */
/** bit0=PWR_EN,1=RST,2=BOOT0,3=VDD33USB,4=SOM_BOOTED input                   */
uint8_t TRES_Seq_GetPinsSnapshot(void);

#ifdef __cplusplus
}
#endif

#endif /* __TRES_SEQ_H__ */