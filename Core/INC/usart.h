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
#include "gpio.h"
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN defines */
/* USART2 runs permanently as TX+RX (PA0 = TX push-pull AF9, PA1 = RX AF9).
 * Who drives the SoC RX line is decided by an external SPDT switch on PF3,
 * NOT by the peripheral:
 *   UART2_TX_SW = 1 : CH343 TX  -> SoC RX (default; host owns the console)
 *   UART2_TX_SW = 0 : PA0 (PY32) -> SoC RX (PY32 sends login/amixer commands)
 * The switch keeps the two TX drivers physically apart, so neither the GPIO
 * mux nor the transfer direction ever needs to change at runtime. */
/* USER CODE END defines */

void Studio_USART2_Init(void);
void Studio_USART1_Init(void);

/* USER CODE BEGIN Prototypes */
/** SPDT TX-source selection for USART2_SetTxSource(). PF3 switch level:
 *  1 = CH343 drives the SoC RX line (default), 0 = PY32 PA0 drives it. */
#define USART2_TXSRC_CH343  (1u)
#define USART2_TXSRC_PY32   (0u)

/**
 * @brief Route the SoC RX line to the wanted TX driver via the PF3 SPDT.
 * @param src USART2_TXSRC_CH343 or USART2_TXSRC_PY32. Safe to call repeatedly.
 * @note Do not switch away from PY32 while a TX DMA transfer is in flight
 *       (USART2_DMA_TxBusy()); the boot FSM's stage dwell times guarantee it.
 */
void USART2_SetTxSource(uint8_t src);
/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */
