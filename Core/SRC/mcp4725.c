/**
 * @file mcp4725.c
 * @brief MCP4725A0 software-I2C driver.
 */
#include "mcp4725.h"

#define MCP4725_CMD_FAST       (0x00U)
#define MCP4725_CMD_EEPROM     (0x60U)
#define MCP4725_CMD_DAC        (0x40U)

static MCP4725_Status_t mcp_status(SoftI2C_Status_t status)
{
    return (MCP4725_Status_t)status;
}

MCP4725_Status_t MCP4725_Init(void)
{
    /* Move the DAC off its EEPROM power-up value to a known code. NOTE: in
     * the inverted FB-injection topology 0 V is the FULL-cooling code, so
     * this alone is not a safe park position - it is covered by TEC_VEN=0
     * here, and TecControl_Init() later parks the DAC on the zero-current
     * end (DAC_MAX_CTRL_MV) once the NVRAM window is loaded. */
    return MCP4725_SetVoltage_mV(0U);
}

MCP4725_Status_t MCP4725_SetCode(uint16_t code)
{
    uint8_t data[2];

    if (code > MCP4725_MAX_CODE) {
        code = MCP4725_MAX_CODE;
    }

    /* Fast mode: 00 PD1 PD0 D11 D10 D9 D8, followed by D7..D0. */
    data[0] = (uint8_t)(MCP4725_CMD_FAST | ((code >> 8U) & 0x0FU));
    data[1] = (uint8_t)(code & 0xFFU);
    return mcp_status(SoftI2C_Master_Write(MCP4725_I2C_ADDR, data, 2U));
}

MCP4725_Status_t MCP4725_SetVoltage_mV(uint16_t millivolts)
{
    uint32_t code;

    if (millivolts >= MCP4725_VDD_MV) {
        code = MCP4725_MAX_CODE;
    } else {
        code = ((uint32_t)millivolts * MCP4725_MAX_CODE) /
               MCP4725_VDD_MV;
    }
    return MCP4725_SetCode((uint16_t)code);
}

MCP4725_Status_t MCP4725_WriteEEPROM(uint16_t code)
{
    uint8_t data[2];

    if (code > MCP4725_MAX_CODE) {
        code = MCP4725_MAX_CODE;
    }

    /* Write DAC register and EEPROM, normal power-down mode. */
    data[0] = (uint8_t)(MCP4725_CMD_EEPROM | ((code >> 8U) & 0x0FU));
    data[1] = (uint8_t)(code & 0xFFU);
    return mcp_status(SoftI2C_Master_Write(MCP4725_I2C_ADDR, data, 2U));
}
