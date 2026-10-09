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
 *   0x30 BOOTSTATE R  SDM845 boot FSM state (Sdm845_State_t, sdm845_boot.h)
 *   0x31 BOOTRETRY R  SDM845 boot retry counter (Sdm845_Boot_GetRetries)
 *   0x32 TECFLAGS  R  TEC control alarms (TEC_ALARM_* bitmask, tec_control.h)
 *   0x33 AHTSTAT   R  last AHT20 driver result code (0 OK, negative = error)
 *   0x34 FANDUTY   R  last fan duty commanded by any writer, percent
 *   0x35 TECDUTY   R  last TEC current command to the DAC, percent
 *   0x40..0xAF    RW  NVRAM parameter window: one 4-byte big-endian value per
 *                     slot. Slot n of NvParamTecId_t (nv_params.h) lives at
 *                     0x40 + 4*n. Values are 32-bit big-endian; writing the
 *                     4th byte commits the value into the NVRAM RAM cache.
 *                     Persist with the SAVE_PARAMS command.
 *                     (0x40 + 4*28 = 0xB0, so the window is 0x40..0xAF.)
 ******************************************************************************
 */
#ifndef __TRES_I2C_SLAVE_H__
#define __TRES_I2C_SLAVE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>
#include "nv_params.h"   /* NVM_PARAM_COUNT for the NVRAM window size */

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

/* Machine-state block (0x30..0x35, all read-only, one byte each): the live
 * state/health of every state machine and actuator on the MCU, so the host
 * can diagnose the whole board from one 6-byte burst read at 0x30. */
#define TRES_REG_BOOTSTATE  0x30u   /**< Sdm845_State_t (sdm845_boot.h)       */
#define TRES_REG_BOOTRETRY  0x31u   /**< SDM845 boot retry counter            */
#define TRES_REG_TECFLAGS   0x32u   /**< TEC_ALARM_* bitmask (tec_control.h)  */
#define TRES_REG_AHTSTAT    0x33u   /**< last AHT20 driver status (int8)      */
#define TRES_REG_FANDUTY    0x34u   /**< last commanded fan duty, percent     */
#define TRES_REG_TECDUTY    0x35u   /**< TEC current cmd to DAC, percent      */

/* NVRAM parameter window: slot n (NvParamTecId_t) at 0x40 + 4*n, 4 bytes
 * big-endian. Reading any byte of a slot returns that byte; writing the 4th
 * byte commits the assembled value into the NVRAM RAM cache. The window is
 * 4 * NVM_PARAM_COUNT bytes, i.e. 0x40..0xAF. */
#define TRES_REG_NV_BASE    0x40u
#define TRES_REG_NV_LEN     4u
#define TRES_REG_NV_END     ( TRES_REG_NV_BASE + TRES_REG_NV_LEN * NVM_PARAM_COUNT )

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
