/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    adc.h
  * @brief   This file provides code for the configuration
  *          of all used ADC.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2026 Puya Semiconductor Co.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by Puya under BSD 3-Clause license,
  * the License ; You may not use this file except in compliance with the
  * License.You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2016 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under BSD 3-Clause license
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __ADC_H__
#define __ADC_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "ntc3950.h"
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN defines */
/* USER CODE END defines */

#define TEC_COLD_TEMP_Pin LL_GPIO_PIN_4
#define TEC_COLD_TEMP_Port GPIOA
#define TEC_HOT_TEMP_Pin LL_GPIO_PIN_5
#define TEC_HOT_TEMP_Port GPIOA
#define TEC_CURR_Pin LL_GPIO_PIN_6
#define TEC_CURR_Port GPIOA

/* INA181A2 current-sense chain. The electrical constants (shunt resistance,
 * amplifier gain, divider ratio, ADC reference) are calibration data and live
 * in NVRAM (NV_TEC_CURRENT_* slots); the scaling function loads them once at
 * first call. See adc.c Studio_ADC1_Init/ReadCurrentMilliamp. */

void Studio_ADC1_Init(void);
uint16_t Studio_ADC1_ReadChannel(uint32_t channel);
uint16_t Studio_ADC1_ReadHotRaw(void);
uint16_t Studio_ADC1_ReadColdRaw(void);
uint16_t Studio_ADC1_ReadCurrentRaw(void);
uint32_t Studio_ADC1_ReadCurrentMilliamp(void);
int32_t Studio_ADC1_ReadHotMilliCelsius(void);
int32_t Studio_ADC1_ReadColdMilliCelsius(void);

/* USER CODE BEGIN Prototypes */

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __ADC_H__ */
