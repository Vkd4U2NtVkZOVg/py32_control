/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    adc.c
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

/* Includes ------------------------------------------------------------------*/
#include "adc.h"
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
static void Studio_ADC1_Calibrate(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* External variables --------------------------------------------------------*/
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/**
  * @brief  Sort the array.
  * @param  arr the array
  * @param  length the array length
  * @retval  None
  */
static void Studio_ADC1_QuickSort(int32_t arr[], int32_t length)
{
  int32_t pos,min;
  for(int32_t i=0;i<length;i++)
  {
    min = arr[i];
    pos = i;
    for(int32_t j=i+1;j<length;j++)
    {
      if(min > arr[j])
      {
        min = arr[j];
        pos = j;
      }
    }
    arr[pos] = arr[i];
    arr[i] = min;
  }
}

/**
  * @brief  ADC calibration function
  * @param  None
  * @retval None
  */
static void Studio_ADC1_Calibrate(void)
{
  __IO uint32_t backup_setting_adc_dma_transfer = 0;
  uint32_t ADC_CALRR1_Buff[5];
  int32_t ADC_CALRR1_BuffInt[5];
#if (USE_TIMEOUT == 1)
  uint32_t Timeout = 0;
#endif

  if (LL_ADC_IsEnabled(ADC1) == 0)
  {
#if (defined(DMA) || defined(DMA1))
    /* Disable ADC DMA transfer during calibration */
    backup_setting_adc_dma_transfer = LL_ADC_REG_GetDMATransfer(ADC1);
    LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_NONE);
#endif

    for(int i=0;i<5;i++)
    {
      /* Enable calibration */
      LL_ADC_StartCalibration(ADC1);

#if (USE_TIMEOUT == 1)
    Timeout = ADC_CALIBRATION_TIMEOUT_MS;
#endif

      while (LL_ADC_IsCalibrationOnGoing(ADC1) != 0)
      {
#if (USE_TIMEOUT == 1)
        /* Check if calibration is timeout */
        if (LL_SYSTICK_IsActiveCounterFlag())
        {
          if(Timeout-- == 0)
          {

          }
        }
#endif
     }
      ADC_CALRR1_Buff[i]= *((__IO uint32_t *)(0x40012448));
      ADC_CALRR1_BuffInt[i]=(int32_t)(ADC_CALRR1_Buff[i]<<9);
   }

    Studio_ADC1_QuickSort(ADC_CALRR1_BuffInt,5);
    *((__IO uint32_t *)(0x40012450)) = (ADC_CALRR1_BuffInt[2]>>9);
    *((__IO uint32_t *)(0x40012454)) = *((__IO uint32_t *)(0x4001244C));
    *((__IO uint32_t *)(0x40012444)) |= 0x8000;
    /* Delay between ADC calibration end and ADC enable: minimum 4 ADC Clock cycles */
    LL_mDelay(1);

#if (defined(DMA) || defined(DMA1))
    /* Restore ADC DMA configuration */
    LL_ADC_REG_SetDMATransfer(ADC1, backup_setting_adc_dma_transfer);
#endif
  }
}

/***************************************
 * @brief ADC1 Init
 **************************************/
void Studio_ADC1_Init(void)
{
  /* USER CODE BEGIN Studio_ADC1_Init 0 */

  /* USER CODE END Studio_ADC1_Init 0 */

  LL_ADC_InitTypeDef ADC_InitStruct = {0};
  LL_ADC_REG_InitTypeDef ADC_REG_InitStruct = {0};
  LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
  /**ADC1 GPIO Configuration
   *PA4     ------> ADC1_IN4
   *PA5     ------> ADC1_IN5
   *PA6     ------> ADC1_IN6
   */
  GPIO_InitStruct.Pin = TEC_COLD_TEMP_Pin|TEC_HOT_TEMP_Pin|TEC_CURR_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  LL_ADC_Reset(ADC1);
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_ADC1);

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  ADC_InitStruct.Clock = LL_ADC_CLOCK_SYNC_PCLK_DIV1;
  ADC_InitStruct.Resolution = LL_ADC_RESOLUTION_12B;
  ADC_InitStruct.DataAlignment = LL_ADC_DATA_ALIGN_RIGHT;
  ADC_InitStruct.LowPowerMode = LL_ADC_LP_MODE_NONE;
  LL_ADC_Init(ADC1, &ADC_InitStruct);

  ADC_REG_InitStruct.TriggerSource = LL_ADC_REG_TRIG_SOFTWARE;
  ADC_REG_InitStruct.SequencerDiscont = LL_ADC_REG_SEQ_DISCONT_DISABLE;
  ADC_REG_InitStruct.ContinuousMode = LL_ADC_REG_CONV_SINGLE;
  ADC_REG_InitStruct.DMATransfer = LL_ADC_REG_DMA_TRANSFER_NONE;
  ADC_REG_InitStruct.Overrun = LL_ADC_REG_OVR_DATA_PRESERVED;
  LL_ADC_REG_Init(ADC1, &ADC_REG_InitStruct);

  LL_ADC_REG_SetSequencerScanDirection(ADC1, LL_ADC_REG_SEQ_SCAN_DIR_FORWARD);
  LL_ADC_SetSamplingTimeCommonChannels(ADC1, LL_ADC_SAMPLINGTIME_3CYCLES_5);

  LL_ADC_REG_SetSequencerChAdd(ADC1, LL_ADC_CHANNEL_4);

  LL_ADC_REG_SetSequencerChAdd(ADC1, LL_ADC_CHANNEL_5);

  LL_ADC_REG_SetSequencerChAdd(ADC1, LL_ADC_CHANNEL_6);

  Studio_ADC1_Calibrate();
  LL_ADC_Enable(ADC1);
  /* USER CODE BEGIN Studio_ADC1_Init 1 */

  /* USER CODE END Studio_ADC1_Init 1 */
}

/*---------------------------------------------------------------------------*/
/* Single-channel conversion helpers. A single software-triggered conversion
 * is used so each result is explicitly associated with its requested channel. */
uint16_t Studio_ADC1_ReadChannel(uint32_t channel)
{
  uint32_t timeout = 100000U;
  uint16_t value = 0U;

  if (LL_ADC_IsEnabled(ADC1) != 0U) {
    LL_ADC_Disable(ADC1);
    while (LL_ADC_IsEnabled(ADC1) != 0U) {
      /* wait for the ADC disable handshake */
    }
  }

  LL_ADC_REG_SetSequencerChannels(ADC1, channel);
  LL_ADC_ClearFlag_EOC(ADC1);
  LL_ADC_ClearFlag_EOS(ADC1);
  LL_ADC_Enable(ADC1);
  LL_ADC_REG_StartConversion(ADC1);
  timeout = 100000U;
  while ((LL_ADC_IsActiveFlag_EOC(ADC1) == 0U) && (timeout-- != 0U)) {
    /* wait for end of conversion */
  }
  if (timeout != 0U) {
    value = LL_ADC_REG_ReadConversionData12(ADC1);
  }
  LL_ADC_ClearFlag_EOC(ADC1);
  LL_ADC_ClearFlag_EOS(ADC1);
  return value;
}

uint16_t Studio_ADC1_ReadHotRaw(void)
{
  return Studio_ADC1_ReadChannel(LL_ADC_CHANNEL_5);
}

uint16_t Studio_ADC1_ReadColdRaw(void)
{
  return Studio_ADC1_ReadChannel(LL_ADC_CHANNEL_4);
}

uint16_t Studio_ADC1_ReadCurrentRaw(void)
{
  return Studio_ADC1_ReadChannel(LL_ADC_CHANNEL_6);
}

uint32_t Studio_ADC1_ReadCurrentMilliamp(void)
{
  uint32_t adc_code = Studio_ADC1_ReadCurrentRaw();
  uint64_t adc_voltage_uV;
  uint64_t shunt_voltage_uV;
  uint64_t current_mA;

  /* Vadc = code * Vref / 4095; undo the INA output divider, then divide by
   * (shunt resistance * amplifier gain). Keep integer arithmetic in uV. */
  adc_voltage_uV = ((uint64_t)adc_code * TEC_CURRENT_ADC_VREF_MV * 1000ULL) /
                   4095ULL;
  shunt_voltage_uV = (adc_voltage_uV * TEC_CURRENT_DIVIDER_DEN) /
                     TEC_CURRENT_DIVIDER_NUM;
  current_mA = (shunt_voltage_uV * 1000ULL) /
               ((uint64_t)TEC_CURRENT_SHUNT_UOHM * TEC_CURRENT_AMP_GAIN);
  return (uint32_t)current_mA;
}

int32_t Studio_ADC1_ReadHotMilliCelsius(void)
{
  return NTC3950_AdcToMilliCelsius(Studio_ADC1_ReadHotRaw());
}

int32_t Studio_ADC1_ReadColdMilliCelsius(void)
{
  return NTC3950_AdcToMilliCelsius(Studio_ADC1_ReadColdRaw());
}

/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
