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

typedef struct {
    int32_t temperature_milli_celsius;
    uint32_t humidity_milli_percent;
} AHT20_Data_t;

int AHT20_Init(void);
int AHT20_IsCalibrated(uint8_t *calibrated);
int AHT20_Read(AHT20_Data_t *data);
int AHT20_ReadRaw(uint32_t *humidity_raw, int32_t *temperature_raw);

#ifdef __cplusplus
}
#endif
#endif /* __AHT20_H__ */