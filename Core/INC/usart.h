/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.h
  * @brief   This file provides code for the configuration
  *          of all used USART.
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
#ifndef __USART_H__
#define __USART_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN defines */
/* USART2 line topology is switched at RUNTIME by USART2_SetLineMode():
 *   rx_only = 1 : PA0 released to high-impedance floating input and the USART
 *                 runs receiver-only. Use while a CH343 USB-UART bridge is
 *                 wired in parallel (a push-pull drive would fight its TX).
 *   rx_only = 0 : PA0 muxed as USART2_TX (push-pull), TX+RX enabled, for
 *                 sending commands to the SoC.
 * Studio_USART2_Init() starts in RX-only; call USART2_SetLineMode() any time
 * to switch. PA1 (RX) is untouched: it is always muxed as USART2_RX. */
/* USER CODE END defines */

void Studio_USART2_Init(void);
void Studio_USART1_Init(void);

/* USER CODE BEGIN Prototypes */
/**
 * @brief Switch the USART2 line topology at runtime.
 * @param rx_only 1 = RX-only (PA0 high-Z, receiver only, CH343 shared);
 *                0 = TX+RX (PA0 muxed as USART2_TX push-pull).
 * PA1 (RX) is always left muxed to USART2. Safe to call repeatedly.
 */
void USART2_SetLineMode(uint8_t rx_only);
/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */
