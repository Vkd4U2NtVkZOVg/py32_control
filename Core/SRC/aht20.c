/**
 * @file aht20.c
 * @brief AHT20 software-I2C driver.
 */
#include <stddef.h>
#include <limits.h>
#include "aht20.h"

#define AHT20_CMD_INIT           (0xBEU)
#define AHT20_CMD_TRIGGER        (0xACU)
#define AHT20_CMD_SOFT_RESET     (0xBAU)

static uint8_t aht20_crc8(const uint8_t *data, uint8_t length)
{
    uint8_t crc = 0xFFU;
    uint8_t i;
    uint8_t bit;

    for (i = 0U; i < length; i++) {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; bit++) {
            crc = ((crc & 0x80U) != 0U) ?
                  (uint8_t)((crc << 1U) ^ 0x31U) : (uint8_t)(crc << 1U);
        }
    }
    return crc;
}

static int aht20_status(SoftI2C_Status_t status)
{
    if (status == SOFT_I2C_OK) {
        return AHT20_OK;
    }
    if (status == SOFT_I2C_ERR_BUSY) {
        return AHT20_ERR_BUSY;
    }
    return AHT20_ERR_BUS;
}

int AHT20_Init(void)
{
    uint8_t status;
    uint8_t command[3] = { AHT20_CMD_INIT, 0x08U, 0x00U };
    SoftI2C_Status_t bus_status;

    /* Read the status byte once: if the calibration bits (0x18) are already
     * set the sensor is ready and no init is needed (datasheet 5.4 step 1:
     * calibration is checked once, at power-up only). If not, issue the
     * standard AHT20 init command. No wait is done here -- the caller/state
     * machine owns the settle delay, so this function is non-blocking. */
    bus_status = SoftI2C_Master_Read(AHT20_I2C_ADDR, &status, 1U);
    if (bus_status != SOFT_I2C_OK) {
        return aht20_status(bus_status);
    }
    if ((status & AHT20_STATUS_CALIBRATED) != AHT20_STATUS_CALIBRATED) {
        bus_status = SoftI2C_Master_Write(AHT20_I2C_ADDR, command, 3U);
        if (bus_status != SOFT_I2C_OK) {
            return aht20_status(bus_status);
        }
    }
    return AHT20_OK;
}

int AHT20_IsCalibrated(uint8_t *calibrated)
{
    uint8_t status;
    SoftI2C_Status_t bus_status;

    if (calibrated == NULL) {
        return AHT20_ERR_PARAM;
    }
    bus_status = SoftI2C_Master_Read(AHT20_I2C_ADDR, &status, 1U);
    if (bus_status != SOFT_I2C_OK) {
        return aht20_status(bus_status);
    }
    *calibrated = ((status & AHT20_STATUS_CALIBRATED) == AHT20_STATUS_CALIBRATED)
                  ? 1U : 0U;
    return AHT20_OK;
}

int AHT20_ReadStatus(uint8_t *status)
{
    SoftI2C_Status_t bus_status;

    if (status == NULL) {
        return AHT20_ERR_PARAM;
    }
    bus_status = SoftI2C_Master_Read(AHT20_I2C_ADDR, status, 1U);
    return aht20_status(bus_status);
}

int AHT20_StartMeasure(void)
{
    uint8_t command[3] = { AHT20_CMD_TRIGGER, 0x33U, 0x00U };
    SoftI2C_Status_t bus_status;

    /* Only the trigger write runs here (~0.1 ms). The datasheet conversion
     * time (~80 ms) is owned by the caller/state machine, not this function,
     * so nothing blocks. */
    bus_status = SoftI2C_Master_Write(AHT20_I2C_ADDR, command, 3U);
    return aht20_status(bus_status);
}

int AHT20_ReadResult(AHT20_Data_t *data)
{
    uint8_t response[7];
    uint32_t humidity_raw;
    int32_t temperature_raw;
    SoftI2C_Status_t bus_status;

    if (data == NULL) {
        return AHT20_ERR_PARAM;
    }

    bus_status = SoftI2C_Master_Read(AHT20_I2C_ADDR, response, 7U);
    if (bus_status != SOFT_I2C_OK) {
        return aht20_status(bus_status);
    }
    if ((response[0] & AHT20_STATUS_BUSY) != 0U) {
        return AHT20_ERR_NOT_READY;
    }
    if (aht20_crc8(response, 6U) != response[6]) {
        return AHT20_ERR_CRC;
    }

    humidity_raw = ((uint32_t)response[1] << 12U) |
                   ((uint32_t)response[2] << 4U) |
                   ((uint32_t)response[3] >> 4U);
    temperature_raw = (int32_t)(((uint32_t)(response[3] & 0x0FU) << 16U) |
                                ((uint32_t)response[4] << 8U) |
                                response[5]);

    /* humidity_raw is 20-bit; the product needs more than 32 bits. */
    data->humidity_milli_percent =
        (uint32_t)(((uint64_t)humidity_raw * 100000ULL) / 1048575ULL);
    data->temperature_milli_celsius =
        (int32_t)(((int64_t)temperature_raw * 200000LL) / 1048575LL) - 50000;
    return AHT20_OK;
}

/* ln(RH/100) * 1e6, sampled every 5 %RH from 5 % to 100 %. */
static const int32_t s_ln_rh_1e6[20] = {
    -2995732, -2302585, -1897120, -1609438, -1386294,
    -1203973, -1049822, -916291,  -798508,  -693147,
    -597837,  -510826,  -430783,  -356675,  -287682,
    -223144,  -162519,  -105361,  -51293,       0
};

static int32_t aht20_ln_rh_1e6(uint32_t humidity_milli_percent)
{
    uint32_t rh_5m;
    uint32_t index;
    uint32_t remainder;
    int32_t low;
    int32_t high;

    /* The table starts at 5 %RH; clamp lower values to avoid extrapolation. */
    if (humidity_milli_percent < 5000U) {
        humidity_milli_percent = 5000U;
    } else if (humidity_milli_percent > 100000U) {
        humidity_milli_percent = 100000U;
    }

    rh_5m = humidity_milli_percent / 5000U;
    index = rh_5m - 1U;
    remainder = humidity_milli_percent % 5000U;
    if (index >= 19U) {
        return s_ln_rh_1e6[19];
    }

    low = s_ln_rh_1e6[index];
    high = s_ln_rh_1e6[index + 1U];
    return low + (int32_t)(((int64_t)(high - low) * remainder) / 5000LL);
}

int32_t AHT20_DewPointMilliCelsius(int32_t temperature_milli_celsius,
                                    uint32_t humidity_milli_percent)
{
    int64_t gamma_1e6;
    int64_t temperature_term;
    int64_t denominator;

    if (humidity_milli_percent == 0U) {
        return INT32_MIN;
    }

    /* Magnus constants: a=17.62, b=243.12 °C. With T in milli-degrees,
     * gamma is scaled by 1e6: 17.62*T_C*1e6 = 17620000*T_mC/1000;
     * the 1000 is represented by the b/T denominator scaling below. */
    temperature_term = (17620000LL * temperature_milli_celsius) /
                       (243120LL + temperature_milli_celsius);
    gamma_1e6 = (int64_t)aht20_ln_rh_1e6(humidity_milli_percent) + temperature_term;
    denominator = 17620000LL - gamma_1e6;
    if (denominator == 0LL) {
        return INT32_MIN;
    }

    return (int32_t)((243120LL * gamma_1e6) / denominator);
}
