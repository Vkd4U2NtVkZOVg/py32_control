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
/* Who drives the SoC RX line is decided by the PF3 switch (UART2_TX_SW):
 *   UART2_TX_SW = 1 : CH343 TX is ON the shared bus (default; host console)
 *   UART2_TX_SW = 0 : CH343 TX is DISCONNECTED; PY32 PA0 sends commands
 * The current hardware only isolates the CH343 driver - PA0 is still wired
 * in parallel, so PA0's line mode must follow the switch (see the
 * USART2_TXSW_LINEMODE gate below). Once a true SPDT routes the two TX
 * sources exclusively, set that gate to 0 and PF3 alone decides. */
/* USER CODE END defines */

void Studio_USART2_Init(void);
void Studio_USART1_Init(void);

/* USER CODE BEGIN Prototypes */
/** TX-source selection for USART2_SetTxSource(). PF3 switch level:
 *  1 = CH343 TX on the shared bus (default), 0 = CH343 disconnected. */
#define USART2_TXSRC_CH343  (1u)
#define USART2_TXSRC_PY32   (0u)

/** PA0 line-mode reconfiguration gate. Current hardware only disconnects the
 *  CH343 TX from the shared bus (PA0 remains in parallel), so USART2_SetTxSource()
 *  must also re-mux PA0: TX push-pull for PY32, high-Z input + RX-only for
 *  CH343. Define to 0 (or -DUSART2_TXSW_LINEMODE=0) once the real SPDT routes
 *  the two TX sources exclusively; USART2_SetTxSource() then only flips PF3. */
#ifndef USART2_TXSW_LINEMODE
#define USART2_TXSW_LINEMODE  (1)
#endif

/**
 * @brief Select who may drive the SoC RX line.
 * @param src USART2_TXSRC_CH343 or USART2_TXSRC_PY32. Safe to call repeatedly.
 *            With USART2_TXSW_LINEMODE=1 this also reconfigures PA0 (TX
 *            push-pull for PY32; high-Z input + RX-only for CH343).
 * @note Do not switch away from PY32 while a TX DMA transfer is in flight
 *       (USART2_DMA_TxBusy()); the boot FSM's stage dwell times guarantee it.
 */
void USART2_SetTxSource(uint8_t src);
/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */
