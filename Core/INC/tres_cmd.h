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
  * Host register values exposed to the I2C layer:
  *     0x10 STATUS   : bit[3:0]=state, 4=host_ready, 5=fault, 6=busy,
  *                     7=last_cmd_rejected
  *     0x11 RETRIES  : consecutive failure counter
  *     0x12 PINS     : live control-pin snapshot
  *     0x13 LASTERR  : detail code of the last rejected command
  *     0x14 VERSION  : interface version
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
    TRES_CMD_PWR_ON_NORMAL = 1u,
    TRES_CMD_PWR_ON_DFU    = 2u,
    TRES_CMD_POWER_OFF     = 3u,
    TRES_CMD_RESET         = 4u,
    TRES_CMD_REBOOT_NORMAL = 5u,
    TRES_CMD_REPORT_FAIL   = 6u
} TRES_CmdId_t;

#define TRES_CMD_MAGIC          0xA5u   /**< first byte of a command write  */

/** Detail codes reported in register 0x13 (LASTERR). */
#define TRES_ERR_NONE           0u
#define TRES_ERR_BUSY           1u      /**< sequencer inside SHUTDOWN      */
#define TRES_ERR_ILLEGAL        2u      /**< command illegal in this state  */
#define TRES_ERR_MAGIC          3u      /**< command write missed 0xA5      */
#define TRES_ERR_PARAM          4u      /**< bad mode parameter             */

#define TRES_IF_VERSION         0x01u   /**< register-file interface version */

/* Exported functions --------------------------------------------------------*/
void TRES_Cmd_Init(void);                 /**< create the parser task         */

/* Called from the I2C1 ISR context once a full command write is received.    */
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
