/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
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

/* Includes ------------------------------------------------------------------*/
#include "gpio.h"
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN Define */

/* USER CODE END Define */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN Macro */

/* USER CODE END Macro */

/* Public variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Private */

/* USER CODE END Private */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* External variables --------------------------------------------------------*/
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/***************************************
 * @brief GPIO Init
 **************************************/
void Studio_GPIO_Init(void)
{
  /* USER CODE BEGIN Studio_GPIO_Init 0 */

  /* USER CODE END Studio_GPIO_Init 0 */

  LL_EXTI_InitTypeDef EXTI_InitStruct = {0};
  LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOB);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOF);

  LL_GPIO_ResetOutputPin(GPIOA, TRES_RST_Pin|TEC_VEN_Pin|TRES_VDD33USB_CTRL_Pin);
  GPIO_InitStruct.Pin = TRES_RST_Pin|TEC_VEN_Pin|TRES_VDD33USB_CTRL_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_DOWN;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  LL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  LL_GPIO_ResetOutputPin(GPIOB, SOFT_IIC_SDA_Pin|SOFT_IIC_SCL_Pin|CM108B_VOLUP_Pin);
  GPIO_InitStruct.Pin = SOFT_IIC_SDA_Pin|SOFT_IIC_SCL_Pin|CM108B_VOLUP_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_UP;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  LL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  LL_GPIO_SetOutputPin(GPIOB, MIX3_BOOTKEY_Pin|MIX3_RST_Pin);
  GPIO_InitStruct.Pin = MIX3_BOOTKEY_Pin|MIX3_RST_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_UP;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  LL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = FAN_TACH_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = SOM_BOOTED_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_DOWN;
  LL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* Fan power rail enable (FAN_PWR_EN, PA11): push-pull output, default OFF
   * so the fan is unpowered until Fan_Init() runs its startup kick. */
  LL_GPIO_ResetOutputPin( GPIOA, FAN_PWR_EN_Pin );
  GPIO_InitStruct.Pin = FAN_PWR_EN_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_DOWN;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  LL_GPIO_Init( GPIOA, &GPIO_InitStruct );
  
  LL_GPIO_ResetOutputPin(GPIOB, TRES_BOOT0_Pin|TRES_PWR_EN_Pin);
  GPIO_InitStruct.Pin = TRES_BOOT0_Pin|TRES_PWR_EN_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_DOWN;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  LL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  LL_GPIO_SetOutputPin(GPIOF, UART2_TX_SW);
  GPIO_InitStruct.Pin = UART2_TX_SW;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_UP;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  LL_GPIO_Init(GPIOF, &GPIO_InitStruct);

  EXTI_InitStruct.Line = LL_EXTI_LINE_3;
  EXTI_InitStruct.Mode = LL_EXTI_MODE_IT;
  EXTI_InitStruct.Trigger = LL_EXTI_TRIGGER_RISING;
  EXTI_InitStruct.LineCommand = ENABLE;
  LL_EXTI_Init(&EXTI_InitStruct);
  LL_EXTI_SetEXTISource(LL_EXTI_CONFIG_PORTB, LL_EXTI_CONFIG_LINE3);
  LL_EXTI_EnableIT(LL_EXTI_LINE_3);
  NVIC_SetPriority(EXTI2_3_IRQn, 1U);
  NVIC_EnableIRQ(EXTI2_3_IRQn);

  /* USER CODE BEGIN Studio_GPIO_Init 1 */
  /* TRES_PWR_EN is left low here on purpose: the TRES sequencer raises it in
     the PWR_UP state entry (Pins_PwrOn), matching the state diagram and its
     anti-backfeed intent. RST stays low (asserted) until BOOT_WAIT releases it. */
  /* USER CODE END Studio_GPIO_Init 1 */
}

/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/* GPIO-only fatal shutdown: independent of scheduler, DAC and interrupts. */
void Board_FatalShutdown(void)
{
    LL_GPIO_InitTypeDef pin = {0};
    __disable_irq();
    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
    LL_GPIO_ResetOutputPin(TEC_VEN_Port, TEC_VEN_Pin);
    pin.Pin = TEC_VEN_Pin;
    pin.Mode = LL_GPIO_MODE_OUTPUT;
    pin.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    pin.Pull = LL_GPIO_PULL_NO;
    pin.Speed = LL_GPIO_SPEED_FREQ_LOW;
    LL_GPIO_Init(TEC_VEN_Port, &pin);
}

