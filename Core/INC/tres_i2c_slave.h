/**
  ******************************************************************************
  * @file    tres_i2c_slave.h
  * @brief   TRES host interface: I2C1 slave register file (layer 1).
  *
  * The I2C1 peripheral is already configured as slave (address 0xA0, PA2/PA3)
  * by Studio_I2C1_Init(); this module only enables its interrupts and serves
  * the transaction protocol in the I2C1 IRQ handler:
  *
  *   Write:  [regPtr] [data ...]      first byte after the address selects
  *                                    the register pointer, following bytes
  *                                    are data. Writing 3 bytes to REG_CMD
  *                                    (0x00) issues a host command:
  *                                    [0xA5][cmdId][param].
  *   Read:   [regPtr] (write) RS [data ...]
  *                                    register pointer auto-increments while
  *                                    the master keeps clocking.
  *
 * Registers:
 *   0x00 CMD      W  command port (magic 0xA5 + TRES_CmdId_t + mode param)
 *   0x10 STATUS   R  bit[3:0]=state 4=host_ready 5=fault 6=busy 7=last_err
 *   0x11 RETRIES  R  consecutive failure counter
 *   0x12 PINS     R  control-pin snapshot
 *   0x13 LASTERR  R  detail code of the last rejected command
 *   0x14 VERSION  R  interface version (TRES_IF_VERSION)
 *   0x20 SDM845_TEMP RW 4-byte window, SDM845 temperature as int32
 *                    milli-degrees Celsius, written MSB first (big endian).
 *                    The value publishes atomically once all 4 bytes have
 *                    arrived; readers use TRES_I2C_Slave_GetSdm845Temp*().
 ******************************************************************************
  */
#ifndef __TRES_I2C_SLAVE_H__
#define __TRES_I2C_SLAVE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>

/* Exported constants --------------------------------------------------------*/
#define TRES_REG_CMD        0x00u
#define TRES_REG_STATUS     0x10u
#define TRES_REG_RETRIES    0x11u
#define TRES_REG_PINS       0x12u
#define TRES_REG_LASTERR    0x13u
#define TRES_REG_VERSION    0x14u

/* SDM845 temperature window: 4 bytes at 0x20..0x23, int32 milli-degree
 * Celsius, transferred MSB first (big endian). Publishes atomically when the
 * 4th byte lands. */
#define TRES_REG_SDM845_TEMP      0x20u
#define TRES_REG_SDM845_TEMP_LEN  4u

/* Exported functions --------------------------------------------------------*/
/** Enable I2C1 slave interrupts (call after TRES_Cmd_Init). */
void TRES_I2C_Slave_Init(void);

/**
 * @brief Temperature the host last wrote to the SDM845_TEMP window.
 * @return int32 milli-degrees Celsius (big-endian value as received).
 */
int32_t TRES_I2C_Slave_GetSdm845TempMilliC(void);

/**
 * @brief Non-zero once the host has completed at least one temperature write.
 *        Reads before the first complete write return 0 milli-Celsius.
 */
uint8_t TRES_I2C_Slave_GetSdm845TempValid(void);

#ifdef __cplusplus
}
#endif

#endif /* __TRES_I2C_SLAVE_H__ */
