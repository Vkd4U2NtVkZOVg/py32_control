/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    main.c
  * @brief   This file provides code for the configuration
  *          of all used MAIN.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2026 Puya Semiconductor Co.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by Mcu Studio under BSD 3-Clause license,
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
#include "main.h"

#if defined(__GNUC__)
/* Keep every function in this translation unit unoptimized for bring-up. */
#pragma GCC optimize ("O0")
#endif

#include "rcc.h"
#include "gpio.h"
#include "i2c.h"
#include "soft_i2c.h"
#include "adc.h"
#include "usart.h"
#include "tim.h"
#include "fan.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "QuarkTS.h"
#include "fsm_demo.h"
#include "qport_py32.h"
#include "notification_demo.h"
#include "tres_seq.h"
#include "tres_cmd.h"
#include "tres_i2c_slave.h"
#include "aht20_task.h"
#include "fan_task.h"
#include "nv_params.h"
#include "usart2_dma.h"
#include "sdm845_detect.h"
#include "sdm845_boot.h"
#include "tec_control.h"
#include "mcp4725.h"
#include "stdio.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* External functions --------------------------------------------------------*/
/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */
  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SYSCFG);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
  LL_PWR_EnableBkUpAccess();
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */
  

  /* Configure the system clock */
  Studio_RCC_Init();
  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  Studio_GPIO_Init();
  Studio_I2C1_Init();
  NvParams_Init();          /* load non-volatile parameters (FLASH, no write) */
  SoftI2C_Init();
  MCP4725_Init();               /* DAC out to 0V: TEC current only by control task */
  Studio_ADC1_Init();
  Studio_USART2_Init();
  USART2_DMA_Start();       /* USART2 (PA0/PA1) DMA RX via DMA1_Channel1 */
  Studio_TIM3_Init();
  Fan_Init();
  Studio_USART1_Init();
  /* USER CODE BEGIN 2 */
  /* Enable the 1 kHz SysTick tick + critical sections (QPort_Setup); the
     scheduler needs this time base live before qOS_Setup is called. */
  QPort_Setup();

  /* Start the scheduler: tick source = QOS_Tick_Get (1 kHz SysTick timebase) */
  qOS_Setup( QOS_Tick_Get, (qTimingBase_t)0.001f, NULL );

  /* State-machine demo (registers its own state-machine + controller tasks) */
  //FSM_Example_Init();
  //Notification_Demo_Init();

  /* TRES power/boot sequencer + Linux-host I2C interface.               */
  /* Order matters: the command parser task must exist before the I2C    */
  /* slave IRQ is enabled (its ISR posts into the parser mailbox).       */
  TRES_Cmd_Init();
  TRES_Seq_Init();          /* registers the FSM task, injects autoboot */
  TRES_I2C_Slave_Init();
  AHT20_Task_Init();        /* 10 s periodic AHT20 dew-point measurement */
  Fan_Task_Init();          /* 2 s periodic fan RPM measurement */
  Sdm845_Detect_Init();     /* SDM845 boot-success detect (USART2 DMA shell) */
  Sdm845_Boot_Init();       /* SDM845 boot/reset/strap state machine */
  TecControl_Init();        /* 1 s TEC cooling-fan control (PID + protection) */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while(1)
  {
    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
    qOS_Run();
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
