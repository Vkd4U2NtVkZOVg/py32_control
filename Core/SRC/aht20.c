/**
 * @file aht20.c
 * @brief AHT20 software-I2C driver.
 */
#include <stddef.h>
#include "aht20.h"
#include "qport_py32.h"

#define AHT20_CMD_INIT           (0xBEU)
#define AHT20_CMD_TRIGGER        (0xACU)
#define AHT20_CMD_SOFT_RESET     (0xBAU)
#define AHT20_STATUS_BUSY        (0x80U)
#define AHT20_STATUS_CALIBRATED  (0x08U)
#define AHT20_MEASURE_WAIT_MS    (80U)
#define AHT20_INIT_WAIT_MS       (10U)
#define AHT20_RESET_WAIT_MS      (20U)

static void aht20_delay_ms(uint32_t delay_ms)
{
    uint32_t start = QOS_Tick_Get();
    while ((uint32_t)(QOS_Tick_Get() - start) < delay_ms) {
        /* cooperative wait; scheduler tick continues in SysTick_Handler */
    }
}

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

    bus_status = SoftI2C_Master_Read(AHT20_I2C_ADDR, &status, 1U);
    if (bus_status != SOFT_I2C_OK) {
        return aht20_status(bus_status);
    }
    if ((status & AHT20_STATUS_CALIBRATED) == 0U) {
        bus_status = SoftI2C_Master_Write(AHT20_I2C_ADDR, command, 3U);
        if (bus_status != SOFT_I2C_OK) {
            return aht20_status(bus_status);
        }
        aht20_delay_ms(AHT20_INIT_WAIT_MS);
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
    *calibrated = ((status & AHT20_STATUS_CALIBRATED) != 0U) ? 1U : 0U;
    return AHT20_OK;
}

int AHT20_ReadRaw(uint32_t *humidity_raw, int32_t *temperature_raw)
{
    uint8_t command[3] = { AHT20_CMD_TRIGGER, 0x33U, 0x00U };
    uint8_t response[7];
    uint32_t humidity;
    int32_t temperature;
    SoftI2C_Status_t bus_status;

    if ((humidity_raw == NULL) || (temperature_raw == NULL)) {
        return AHT20_ERR_PARAM;
    }

    bus_status = SoftI2C_Master_Write(AHT20_I2C_ADDR, command, 3U);
    if (bus_status != SOFT_I2C_OK) {
        return aht20_status(bus_status);
    }
    aht20_delay_ms(AHT20_MEASURE_WAIT_MS);

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

    humidity = ((uint32_t)response[1] << 12U) |
               ((uint32_t)response[2] << 4U) |
               ((uint32_t)response[3] >> 4U);
    temperature = (int32_t)(((uint32_t)(response[3] & 0x0FU) << 16U) |
                             ((uint32_t)response[4] << 8U) |
                             response[5]);
    *humidity_raw = humidity;
    *temperature_raw = temperature;
    return AHT20_OK;
}

int AHT20_Read(AHT20_Data_t *data)
{
    uint32_t humidity_raw;
    int32_t temperature_raw;
    int status;

    if (data == NULL) {
        return AHT20_ERR_PARAM;
    }
    status = AHT20_ReadRaw(&humidity_raw, &temperature_raw);
    if (status != AHT20_OK) {
        return status;
    }

    data->humidity_milli_percent = (humidity_raw * 100000U) / 1048575U;
    data->temperature_milli_celsius =
        (int32_t)(((int64_t)temperature_raw * 200000LL) / 1048575LL) - 50000;
    return AHT20_OK;
}
