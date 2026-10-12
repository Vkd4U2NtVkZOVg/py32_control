/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.c
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

/* Includes ------------------------------------------------------------------*/
#include "usart.h"
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
 * @brief USART1 Init
 **************************************/
void Studio_USART1_Init(void)
{
  /* USER CODE BEGIN Studio_USART1_Init 0 */

  /* USER CODE END Studio_USART1_Init 0 */

  LL_USART_InitTypeDef USART_InitStruct = {0};
  LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
  /**USART1 GPIO Configuration
   *PA9     ------> USART1_TX
   *PA10     ------> USART1_RX
   */
  GPIO_InitStruct.Pin = LL_GPIO_PIN_9|LL_GPIO_PIN_10;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_UP;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Alternate = LL_GPIO_AF1_USART1;
  LL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_USART1);

  USART_InitStruct.BaudRate = 115200;
  USART_InitStruct.DataWidth = LL_USART_DATAWIDTH_8B;
  USART_InitStruct.Parity = LL_USART_PARITY_NONE;
  USART_InitStruct.StopBits = LL_USART_STOPBITS_1;
  USART_InitStruct.TransferDirection = LL_USART_DIRECTION_TX_RX;
  USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;
  USART_InitStruct.OverSampling = LL_USART_OVERSAMPLING_16;
  LL_USART_Init(USART1, &USART_InitStruct);

  LL_USART_DisableAutoBaudRate(USART1);

  LL_USART_ConfigAsyncMode(USART1);

  LL_USART_Enable(USART1);

  /* USER CODE BEGIN Studio_USART1_Init 1 */

  /* USER CODE END Studio_USART1_Init 1 */
}

/***************************************
 * @brief USART2 Init
 **************************************/
void Studio_USART2_Init(void)
{
  /* USER CODE BEGIN Studio_USART2_Init 0 */

  /* USER CODE END Studio_USART2_Init 0 */

  LL_USART_InitTypeDef USART_InitStruct = {0};
  LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
  /**USART2 GPIO Configuration
   * Both pins are muxed TX+RX here (generator-native). While the
   * USART2_TXSW_LINEMODE gate is 1, USART2_SetTxSource() reconfigures PA0
   * at the end of this init: it starts on the CH343 throw, so PA0 goes to
   * a high-Z input (RX-only) until the PY32 sends.
   *PA0     ------> USART2_TX (push-pull AF9 while the PY32 sends)
   *PA1     ------> USART2_RX (always AF9)
   */
  GPIO_InitStruct.Pin = LL_GPIO_PIN_0|LL_GPIO_PIN_1;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_UP;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Alternate = LL_GPIO_AF9_USART2;
  LL_GPIO_Init(GPIOA, &GPIO_InitStruct);


  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);

  USART_InitStruct.BaudRate = 115200;
  USART_InitStruct.DataWidth = LL_USART_DATAWIDTH_8B;
  USART_InitStruct.Parity = LL_USART_PARITY_NONE;
  USART_InitStruct.StopBits = LL_USART_STOPBITS_1;
  USART_InitStruct.TransferDirection = LL_USART_DIRECTION_TX_RX;
  USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;
  USART_InitStruct.OverSampling = LL_USART_OVERSAMPLING_16;
  LL_USART_Init(USART2, &USART_InitStruct);

  LL_USART_DisableAutoBaudRate(USART2);

  LL_USART_ConfigAsyncMode(USART2);

  LL_USART_Enable(USART2);

  /* USER CODE BEGIN Studio_USART2_Init 1 */
  USART2_SetTxSource( USART2_TXSRC_CH343 );  /* default: host owns the SoC RX */
  /* USER CODE END Studio_USART2_Init 1 */
}

/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* USER CODE BEGIN 1 */
/**
  * @brief  Select who may drive the SoC RX line via the PF3 switch
  *         (UART2_TX_SW), which disconnects / reconnects the CH343 TX from
  *         the shared bus.
  *         USART2_TXSRC_CH343 (1): CH343 on the bus (default, host console).
  *         USART2_TXSRC_PY32  (0): CH343 disconnected, PY32 PA0 sends.
  *         While USART2_TXSW_LINEMODE is 1 (current partial hardware: PA0 is
  *         still wired in parallel), this also reconfigures PA0 per throw:
  *         TX push-pull + TX_RX for PY32, high-Z input + RX-only for CH343.
  *         Isolation ORDER matters in that mode: handing the line to the
  *         PY32 weak-holds PA0 first, cuts the CH343, and only then makes
  *         PA0 push-pull - the old config-first order let a CH343 low bit
  *         fight the fresh push-pull driver for the window between the two
  *         steps. With a true SPDT installed, build with
  *         -DUSART2_TXSW_LINEMODE=0 and only PF3 is toggled.
  * @param  src USART2_TXSRC_CH343 or USART2_TXSRC_PY32. Safe to call
  *         repeatedly: a same-state call is a true no-op (no GPIO churn).
  * @note   Never switch away from PY32 while USART2_DMA_TxBusy(): the byte in
  *         flight would be corrupted. The boot FSM only switches at state
  *         entries, where no transfer can be pending.
  * @retval None
  */
static uint8_t s_tx_src = 0xFFu;   /* 0xFF = unset: the first call configures */

void USART2_SetTxSource( uint8_t src )
{
#if USART2_TXSW_LINEMODE
  LL_GPIO_InitTypeDef GPIO_InitStruct;

  if ( src == s_tx_src ) {
    return;   /* repeated same-state call: no GPIO churn, no glitch window */
  }

  if ( src == USART2_TXSRC_PY32 ) {
    /* Isolation order on the parallel hardware (only PF3 can disconnect the
     * CH343): weak-hold PA0 (high-Z + pull-up, no conflict with either
     * driver), THEN cut the CH343, and only then let the final config below
     * make PA0 a push-pull driver. Between the cut and the drive the pull-up
     * holds the line idle-high, so the SoC RX never floats. Handing the line
     * back keeps the reverse order below: PA0 to high-Z before the CH343
     * reconnects. */
    GPIO_InitStruct.Pin        = LL_GPIO_PIN_0;
    GPIO_InitStruct.Mode       = LL_GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull       = LL_GPIO_PULL_UP;
    GPIO_InitStruct.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Alternate  = LL_GPIO_AF9_USART2;
    LL_GPIO_Init( GPIOA, &GPIO_InitStruct );
    LL_GPIO_ResetOutputPin( UART2_TX_SW_Port, UART2_TX_SW );   /* CH343 off */
  }

  /* PA0 only; PA1 stays muxed as USART2_RX in both states. */
  LL_GPIO_StructInit( &GPIO_InitStruct );
  GPIO_InitStruct.Pin        = LL_GPIO_PIN_0;
  GPIO_InitStruct.Mode       = ( src == USART2_TXSRC_PY32 ) ? LL_GPIO_MODE_ALTERNATE
                                                            : LL_GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull       = ( src == USART2_TXSRC_PY32 ) ? LL_GPIO_PULL_UP
                                                            : LL_GPIO_PULL_NO;
  GPIO_InitStruct.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Alternate  = LL_GPIO_AF9_USART2;
  LL_GPIO_Init( GPIOA, &GPIO_InitStruct );

  /* TX+RX while sending; receiver-only while the CH343 owns the bus. */
  LL_USART_SetTransferDirection( USART2,
      ( src == USART2_TXSRC_PY32 ) ? LL_USART_DIRECTION_TX_RX
                                   : LL_USART_DIRECTION_RX );

  if ( src != USART2_TXSRC_PY32 ) {
    LL_GPIO_SetOutputPin( UART2_TX_SW_Port, UART2_TX_SW );  /* CH343 on */
  }
#else
  /* True SPDT: the throw itself is exclusive, so the order does not matter. */
  if ( src == USART2_TXSRC_PY32 ) {
    LL_GPIO_ResetOutputPin( UART2_TX_SW_Port, UART2_TX_SW );
  } else {
    LL_GPIO_SetOutputPin( UART2_TX_SW_Port, UART2_TX_SW );
  }
#endif

  s_tx_src = src;
}
/* USER CODE END 1 */
