/**
 * @file aht20.h
 * @brief AHT20 temperature/humidity sensor driver over SoftI2C.
 */
#ifndef __AHT20_H__
#define __AHT20_H__

#include <stdint.h>
#include "soft_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AHT20_I2C_ADDR
#define AHT20_I2C_ADDR          (0x38U)
#endif

#define AHT20_OK                (0)
#define AHT20_ERR_PARAM        (-1)
#define AHT20_ERR_BUS          (-2)
#define AHT20_ERR_CRC           (-3)
#define AHT20_ERR_BUSY          (-4)
#define AHT20_ERR_NOT_READY     (-5)

/* Status-register bit masks (raw byte read via AHT20_ReadStatus / 0x71). */
#define AHT20_STATUS_BUSY        (0x80U)      /**< Bit[7]: measurement busy    */
#define AHT20_STATUS_CALIBRATED  (0x18U)      /**< Bit[3:4]: calibrated (both set) */

typedef struct {
    int32_t temperature_milli_celsius;
    uint32_t humidity_milli_percent;
} AHT20_Data_t;

int AHT20_Init(void);
int AHT20_IsCalibrated(uint8_t *calibrated);

/**
 * @brief Read the raw status register (0x71) into *status. Non-blocking
 *        (only the bit-banged 1-byte I2C read runs here, ~0.1 ms).
 * @param status Output: raw status byte (Bit[7]=busy, Bit[3:4]=calibrated).
 * @return AHT20_OK, or AHT20_ERR_BUSY / AHT20_ERR_BUS / AHT20_ERR_PARAM.
 */
int AHT20_ReadStatus(uint8_t *status);

/**
 * @brief Send the trigger-measurement command (0xAC) only. Non-blocking:
 *        no wait is performed here; the caller/state machine must wait the
 *        datasheet conversion time (~80 ms) before calling AHT20_ReadResult.
 */
int AHT20_StartMeasure(void);

/**
 * @brief Read and convert the measurement result. Non-blocking (only the
 *        bit-banged I2C transaction runs here, ~0.1 ms).
 * @param data Output temperature/humidity in milli-units.
 * @return AHT20_OK, or AHT20_ERR_BUSY / AHT20_ERR_CRC / AHT20_ERR_BUS /
 *         AHT20_ERR_NOT_READY / AHT20_ERR_PARAM.
 */
int AHT20_ReadResult(AHT20_Data_t *data);

/**
 * @brief Calculate dew-point temperature using fixed-point Magnus approximation.
 * @param temperature_milli_celsius Air temperature in milli-degrees Celsius.
 * @param humidity_milli_percent Relative humidity in milli-percent (0..100000).
 * @return Dew point in milli-degrees Celsius, or INT32_MIN for invalid RH=0.
 *
 * The implementation uses integer arithmetic and a compact ln(RH) table;
 * it does not pull floating-point support into the firmware.
 */
int32_t AHT20_DewPointMilliCelsius(int32_t temperature_milli_celsius,
                                    uint32_t humidity_milli_percent);

#ifdef __cplusplus
}
#endif
#endif /* __AHT20_H__ */