/**
 * @file mcp4725.h
 * @brief MCP4725 12-bit voltage-output DAC driver over SoftI2C.
 */
#ifndef __MCP4725_H__
#define __MCP4725_H__

#include <stdint.h>
#include "soft_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MCP4725A0: A0 low -> 0x60. Change to 0x61 if the schematic straps A0 high. */
#ifndef MCP4725_I2C_ADDR
#define MCP4725_I2C_ADDR       (0x60U)
#endif

#define MCP4725_MAX_CODE       (4095U)

typedef enum {
    MCP4725_OK = 0,
    MCP4725_ERR_PARAM = SOFT_I2C_ERR_PARAM,
    MCP4725_ERR_NACK = SOFT_I2C_ERR_NACK,
    MCP4725_ERR_BUS = SOFT_I2C_ERR_BUSY
} MCP4725_Status_t;

MCP4725_Status_t MCP4725_Init(void);
MCP4725_Status_t MCP4725_SetCode(uint16_t code);
MCP4725_Status_t MCP4725_SetVoltage_mV(uint16_t millivolts, uint16_t vdd_millivolts);
MCP4725_Status_t MCP4725_SetVoltage(uint16_t millivolts, uint16_t vdd_millivolts);
MCP4725_Status_t MCP4725_WriteEEPROM(uint16_t code);

#ifdef __cplusplus
}
#endif
#endif /* __MCP4725_H__ */