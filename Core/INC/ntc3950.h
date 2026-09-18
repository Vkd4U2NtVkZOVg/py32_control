/**
 * @file ntc3950.h
 * @brief 10 kOhm / B3950 NTC resistance-to-temperature conversion.
 */
#ifndef __NTC3950_H__
#define __NTC3950_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NTC3950_TABLE_MIN_C       (-40)
#define NTC3950_TABLE_MAX_C       (160)
#define NTC3950_FIXED_RES_OHM     (10000UL)
#define NTC3950_ADC_FULL_SCALE    (4095UL)
#define NTC3950_SUPPLY_MV         (1800UL)

/* resistance in ohms -> temperature in milli-degrees Celsius */
int32_t NTC3950_ResistanceToMilliCelsius(uint32_t resistance_ohm);

/* 12-bit ADC code for the schematic: 1V8 -> NTC -> node -> 10k -> GND. */
int32_t NTC3950_AdcToMilliCelsius(uint16_t adc_code);

/* ADC code -> NTC resistance, with saturation at the two rails. */
uint32_t NTC3950_AdcToResistance(uint16_t adc_code);

#ifdef __cplusplus
}
#endif
#endif /* __NTC3950_H__ */