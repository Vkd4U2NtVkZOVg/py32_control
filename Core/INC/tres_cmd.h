/**
  ******************************************************************************
  * @file    tres_cmd.h
  * @brief   TRES command policy/parser (layer 2) + host register values.
  *
  * The I2C slave ISR (tres_i2c_slave.c) posts fully received commands here
  * via TRES_Cmd_PostFromISR(); a disabled parser task is woken with a QuarkTS
  * notification (pattern proven by notification_demo.c), validates the
  * command against the legality matrix of the state diagram and forwards it
  * to the sequencer FSM (tres_seq.c) as a signal.
  *
  * CMD_REPORT_FAIL never touches the FSM: it only increments a failure
  * counter, which the host reads back through the register file (this is the
  * "count, do not react" behavior of the state diagram).
  *
  * Host register values exposed to the I2C layer:
 *     0x10 STATUS   : bit[3:0]=state, 4=host_ready, 7=last_cmd_rejected
 *     0x11 RETRIES  : CMD_REPORT_FAIL counter
 *     0x12 PINS     : live control-pin snapshot
 *     0x13 LASTERR  : detail code of the last rejected command
 *     0x14 VERSION  : interface version
 *     0x40..0xC7    : NVRAM parameter window (see tres_i2c_slave.h)
 ******************************************************************************
 */
#ifndef __TRES_CMD_H__
#define __TRES_CMD_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>

/* Exported constants --------------------------------------------------------*/
/** Command codes written by the host into register 0x00 (after 0xA5 magic). */
typedef enum {
    TRES_CMD_RESET       = 1u,  /**< reset pulse, boot NORMAL (BOOT0=0)      */
    TRES_CMD_ENTER_DFU   = 2u,  /**< reset pulse, boot DFU (BOOT0=1, USB on) */
    TRES_CMD_REPORT_FAIL = 3u,  /**< count-only failure report (no reaction) */
    TRES_CMD_SAVE_PARAMS = 4u,  /**< persist the NVRAM RAM cache to FLASH    */
    TRES_CMD_DEFAULT_PARAMS = 5u /**< reload NVRAM firmware defaults (RAM)    */
} TRES_CmdId_t;

#define TRES_CMD_MAGIC          0xA5u   /**< first byte of a command write  */

/** Detail codes reported in register 0x13 (LASTERR). */
#define TRES_ERR_NONE           0u
#define TRES_ERR_ILLEGAL        1u      /**< command illegal in this state  */
#define TRES_ERR_MAGIC          2u      /**< command write missed 0xA5      */

#define TRES_IF_VERSION         0x02u   /**< register-file interface version */

/* Exported functions --------------------------------------------------------*/
void TRES_Cmd_Init(void);                 /**< create the parser task         */

/* Called from the I2C1 ISR context once a full command write is received.    */
/* Note: the third byte (param) of the wire format is ignored; these commands */
/* carry no parameter.                                                        */
void TRES_Cmd_PostFromISR(uint8_t cmd, uint8_t param);
void TRES_Cmd_PostBadMagicFromISR(void);

/* Register values read by the I2C layer (thread/ISR safe, single-byte).      */
uint8_t TRES_Cmd_StatusByte(void);
uint8_t TRES_Cmd_Retries(void);
uint8_t TRES_Cmd_LastErr(void);

#ifdef __cplusplus
}
#endif

#endif /* __TRES_CMD_H__ */
