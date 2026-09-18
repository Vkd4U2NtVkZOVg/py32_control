/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.h
  * @brief   This file provides code for the configuration
  *          of all used GPIO.
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
#ifndef __GPIO_H__
#define __GPIO_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN defines */
/* USER CODE END defines */

#define TRES_RST_Pin LL_GPIO_PIN_8
#define TRES_RST_Port GPIOA
#define FAN_CH_SW_Pin LL_GPIO_PIN_11
#define FAN_CH_SW_Port GPIOA
#define TEC_VEN_Pin LL_GPIO_PIN_12
#define TEC_VEN_Port GPIOA
#define TRES_VDD33USB_CTRL_Pin LL_GPIO_PIN_15
#define TRES_VDD33USB_CTRL_Port GPIOA
/* Schematic net mapping: PB0 = PY_SOFT_IIC_SDA, PB1 = PY_SOFT_IIC_SCL. */
#define SOFT_IIC_SDA_Pin LL_GPIO_PIN_0
#define SOFT_IIC_SDA_Port GPIOB
#define SOFT_IIC_SCL_Pin LL_GPIO_PIN_1
#define SOFT_IIC_SCL_Port GPIOB
#define CM108B_VOLUP_Pin LL_GPIO_PIN_2
#define CM108B_VOLUP_Port GPIOB
#define FAN_TACH_Pin LL_GPIO_PIN_3
#define FAN_TACH_Port GPIOB
#define SOM_BOOTED_Pin LL_GPIO_PIN_4
#define SOM_BOOTED_Port GPIOB
#define TRES_BOOT0_Pin LL_GPIO_PIN_5
#define TRES_BOOT0_Port GPIOB
#define TRES_PWR_EN_Pin LL_GPIO_PIN_6
#define TRES_PWR_EN_Port GPIOB
#define MIX3_BOOTKEY_Pin LL_GPIO_PIN_7
#define MIX3_BOOTKEY_Port GPIOB
#define MIX3_RST_Pin LL_GPIO_PIN_8
#define MIX3_RST_Port GPIOB
#define KEY1_Pin LL_GPIO_PIN_3
#define KEY1_Port GPIOF

void Studio_GPIO_Init(void);

/* USER CODE BEGIN Prototypes */

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __GPIO_H__ */
