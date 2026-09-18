/**
  ******************************************************************************
  * @file    soft_i2c.h
  * @brief   Bit-banged (software) master I2C driver.
  *
  * Implements a full standard / fast I2C *master* over two arbitrary GPIOs
  * (open-drain SDA + push-pull SCL), so the bus can be driven without the
  * on-chip I2C peripheral. This module is NOT tied to a specific device: it
  * exposes low-level bit/byte/transaction primitives plus convenient
  * register read/write helpers.
  *
  * Bus wiring (see gpio.h):
  *     SDA   = PB0  (GPIOB pin 0)  -> open-drain output + pull-up
  *     SCL   = PB1  (GPIOB pin 1)  -> push-pull output
  * The board must provide the SDA pull-up resistor (or rise-time is relaxed
  * by enabling the internal pull-up).
  *
  * Device addresses are passed as 7-bit addresses (as printed in a datasheet,
  * e.g. 0x48). The driver builds the 8-bit R/W byte internally.
  ******************************************************************************
  */
#ifndef __SOFT_I2C_H__
#define __SOFT_I2C_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"

/* Exported types ------------------------------------------------------------*/
/** Driver status codes returned by every public transaction. */
typedef enum {
    SOFT_I2C_OK        = 0U,  /**< Operation completed successfully.           */
    SOFT_I2C_ERR_NACK,        /**< Slave did not acknowledge an address/reg/byte. */
    SOFT_I2C_ERR_BUSY,        /**< Bus not free (SCL/SDA stuck low) at start.  */
    SOFT_I2C_ERR_PARAM        /**< Invalid parameter (NULL buffer / zero len). */
} SoftI2C_Status_t;

/** ACK control supplied to SoftI2C_ReadByte() for the 9th clock pulse. */
typedef enum {
    SOFT_I2C_NACK = 0U,      /**< Master does not ACK -> master stops the read. */
    SOFT_I2C_ACK            /**< Master ACKs -> master keeps reading.          */
} SoftI2C_Ack_t;

/* Exported functions ---------------------------------------------------------*/

/**
 * @brief  Configure the SDA/SCL pins for bit-banged operation.
 * @note   Idle state after Init: SCL high, SDA high (released).
 */
void SoftI2C_Init(void);

/**
 * @brief  Release the SDA/SCL pins (shared-level convenience).
 * @note   The pins are left as outputs/idle; call @ref SoftI2C_Init to reuse.
 */
void SoftI2C_DeInit(void);

/* ---- Low-level sequence primitives (single master, no arbitration) ---- */

/**
 * @brief  Generate an I2C START condition (SDA falls while SCL high).
 * @retval SOFT_I2C_OK    bus was free and START was emitted.
 * @retval SOFT_I2C_ERR_BUSY  bus busy (SCL or SDA already low) - no START.
 */
SoftI2C_Status_t SoftI2C_Start(void);

/**
 * @brief  Generate an I2C STOP condition (SDA rises while SCL high).
 */
void SoftI2C_Stop(void);

/**
 * @brief  Generate a repeated-START condition (for register reads).
 * @retval SoftI2C_Status_t  as @ref SoftI2C_Start.
 */
SoftI2C_Status_t SoftI2C_RepStart(void);

/**
 * @brief  Transmit one byte (MSB first) and read the slave ACK bit.
 * @param  data  Byte to send.
 * @retval SOFT_I2C_OK    slave sent ACK.
 * @retval SOFT_I2C_ERR_NACK   slave sent NACK (address/reg/byte not accepted).
 */
SoftI2C_Status_t SoftI2C_WriteByte(uint8_t data);

/**
 * @brief  Receive one byte (MSB first) and then drive the ACK/NACK clock.
 * @param  out  Out-pointer receiving the byte (must not be NULL).
 * @param  ack  SOFT_I2C_ACK to ACK (continue), SOFT_I2C_NACK to NACK (finish).
 * @retval SOFT_I2C_OK    byte received.
 * @retval SOFT_I2C_ERR_PARAM  @p out was NULL.
 */
SoftI2C_Status_t SoftI2C_ReadByte(uint8_t *out, SoftI2C_Ack_t ack);

/* ---- Higher-level transactions ----------------------------------------- */

/**
 * @brief  Master transmit: START, address+W, @p len bytes, STOP.
 * @param  addr7  7-bit slave address.
 * @param  buf    Data to send (must not be NULL when len > 0).
 * @param  len    Number of bytes to send.
 * @retval SoftI2C_Status_t
 */
SoftI2C_Status_t SoftI2C_Master_Write(uint8_t addr7, const uint8_t *buf, uint16_t len);

/**
 * @brief  Master receive: START, address+R, @p len bytes, STOP.
 * @note   The last byte is read with NACK (normal master termination).
 * @param  addr7  7-bit slave address.
 * @param  buf    Buffer receiving the data (must not be NULL when len > 0).
 * @param  len    Number of bytes to read.
 * @retval SoftI2C_Status_t
 */
SoftI2C_Status_t SoftI2C_Master_Read(uint8_t addr7, uint8_t *buf, uint16_t len);

/**
 * @brief  Write to an 8-bit register: START, addr+W, reg, data..., STOP.
 * @param  addr7  7-bit slave address.
 * @param  reg    8-bit register (register-select) address.
 * @param  buf    Data payload.
 * @param  len    Payload length (0 = only select the register).
 * @retval SoftI2C_Status_t
 */
SoftI2C_Status_t SoftI2C_WriteReg(uint8_t addr7, uint8_t reg, const uint8_t *buf, uint16_t len);

/**
 * @brief  Read from an 8-bit register: START, addr+W, reg, REPSTART,
 *         addr+R, data..., STOP.
 * @param  addr7  7-bit slave address.
 * @param  reg    8-bit register (register-select) address.
 * @param  buf    Buffer receiving the data (must not be NULL when len > 0).
 * @param  len    Number of bytes to read.
 * @retval SoftI2C_Status_t
 */
SoftI2C_Status_t SoftI2C_ReadReg(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len);

/* ---- Convenience single-register helpers ------------------------------- */

/**
 * @brief  Write a single byte to an 8-bit register.
 */
SoftI2C_Status_t SoftI2C_WriteReg8(uint8_t addr7, uint8_t reg, uint8_t val);

/**
 * @brief  Read a single byte from an 8-bit register.
 */
SoftI2C_Status_t SoftI2C_ReadReg8(uint8_t addr7, uint8_t reg, uint8_t *val);

/**
 * @brief  Read a big-endian 16-bit value from an 8-bit register pair
 *         (reg = high byte, reg+1 = low byte), e.g. temperature sensors.
 */
SoftI2C_Status_t SoftI2C_ReadReg16(uint8_t addr7, uint8_t reg, uint16_t *val);

#ifdef __cplusplus
}
#endif

#endif /* __SOFT_I2C_H__ */