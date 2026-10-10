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
#include "nv_params.h"
#include "tres_i2c_slave.h"
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
   *  Clock follows the official LL ADC_MultichannelSwitch example: SYSCLK 32 MHz
   *  / APB1 DIV1 -> PCLK 32 MHz, so DIV4 keeps the ADC clock at 8 MHz (within
   *  spec). DIV1 (32 MHz) exceeded the ADC clock limit and made conversions
   *  unreliable after the first one.
  */
  ADC_InitStruct.Clock = LL_ADC_CLOCK_SYNC_PCLK_DIV4;
  ADC_InitStruct.Resolution = LL_ADC_RESOLUTION_12B;
  ADC_InitStruct.DataAlignment = LL_ADC_DATA_ALIGN_RIGHT;
  ADC_InitStruct.LowPowerMode = LL_ADC_LP_AUTOWAIT;
  LL_ADC_Init(ADC1, &ADC_InitStruct);

  ADC_REG_InitStruct.TriggerSource = LL_ADC_REG_TRIG_SOFTWARE;
  ADC_REG_InitStruct.SequencerDiscont = LL_ADC_REG_SEQ_DISCONT_DISABLE;
  ADC_REG_InitStruct.ContinuousMode = LL_ADC_REG_CONV_SINGLE;
  ADC_REG_InitStruct.DMATransfer = LL_ADC_REG_DMA_TRANSFER_NONE;
  ADC_REG_InitStruct.Overrun = LL_ADC_REG_OVR_DATA_PRESERVED;
  LL_ADC_REG_Init(ADC1, &ADC_REG_InitStruct);

  LL_ADC_REG_SetSequencerScanDirection(ADC1, LL_ADC_REG_SEQ_SCAN_DIR_FORWARD);
  /* Official example uses 239.5 cycles; 3.5 cycles undersampled the NTC/INA
     chain and left the sample capacitor stuck at an arbitrary mid-scale value. */
  LL_ADC_SetSamplingTimeCommonChannels(ADC1, LL_ADC_SAMPLINGTIME_239CYCLES_5);

  Studio_ADC1_Calibrate();
  LL_ADC_Enable(ADC1);
  /* USER CODE BEGIN Studio_ADC1_Init 1 */

  /* USER CODE END Studio_ADC1_Init 1 */
}

/*---------------------------------------------------------------------------*/
/* Single-channel conversion helpers (official LL ADC_MultichannelSwitch     */
/* pattern: APP_AdcConvert). The ADC is started once at init and kept        */
/* enabled - AN1011 §1: "ADC使能后软件不能禁能" (a runtime LL_ADC_Disable made */
/* conversions stick at 2048). Each read re-assigns one channel, starts a     */
/* conversion, waits for EOC, and clears CHSELR again.                        */
uint16_t Studio_ADC1_ReadChannel(uint32_t channel)
{
  uint16_t value = 0U;
  uint32_t timeout = 1000000U;   /* ~ms at 32 MHz; abort if EOC never arrives */

  /* Clear all channels */
  WRITE_REG(ADC1->CHSELR, 0U);
  /* Select the wanted channel */
  LL_ADC_REG_SetSequencerChannels(ADC1, channel);

  LL_ADC_Enable(ADC1);                             /* ADC must remain enabled; no disable */
  LL_mDelay(1);                                    /* ADC stabilization, min 8 ADC clocks */

  LL_ADC_REG_StartConversion(ADC1);
  while (LL_ADC_IsActiveFlag_EOC(ADC1) == 0U) {
    if ( timeout-- == 0U ) {
      /* Conversion never completed (clock/peripheral fault). Return 0: the
       * hot/cold NTC paths then read as sensor faults and the TEC loop
       * drives to a safe state instead of blocking forever. */
      WRITE_REG(ADC1->CHSELR, 0U);
      return 0U;
    }
  }
  LL_ADC_ClearFlag_EOC(ADC1);
  value = LL_ADC_REG_ReadConversionData12(ADC1);

  /* Clear all channels */
  WRITE_REG(ADC1->CHSELR, 0U);

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
  /* Current-sense chain calibration from NVRAM (INA181A2 + divider). Values
   * are snapshotted once; changing NVRAM takes effect after reboot. */
  static uint32_t shunt_uohm = 0u;
  static uint32_t amp_gain   = 0u;
  static uint32_t div_num    = 1u;
  static uint32_t div_den    = 1u;
  static uint32_t adc_vref_mv = 0u;
  uint32_t adc_code = Studio_ADC1_ReadCurrentRaw();
  uint64_t adc_voltage_uV;
  uint64_t shunt_voltage_uV;
  uint64_t current_mA;

  if ( ( shunt_uohm == 0u ) || ( amp_gain == 0u ) || ( div_num == 0u ) ||
       ( div_den == 0u ) || ( adc_vref_mv == 0u ) ) {
    if ( shunt_uohm == 0u ) {
      shunt_uohm = NvParams_Get( NV_TEC_CURRENT_SHUNT_UOHM );
    }
    if ( amp_gain == 0u ) {
      amp_gain = NvParams_Get( NV_TEC_CURRENT_AMP_GAIN );
    }
    if ( div_num == 0u ) {
      div_num = NvParams_Get( NV_TEC_CURRENT_DIVIDER_NUM );
    }
    if ( div_den == 0u ) {
      div_den = NvParams_Get( NV_TEC_CURRENT_DIVIDER_DEN );
    }
    if ( adc_vref_mv == 0u ) {
      adc_vref_mv = NvParams_Get( NV_TEC_CURRENT_ADC_VREF_MV );
    }
    /* Guard against corrupt NVRAM values (including a zero shunt, which
     * would make the denominator below 0 and silently kill overcurrent
     * protection): fall back to the factory defaults, read via
     * NvParams_GetDefault() so they always track s_defaults. */
    if ( ( amp_gain == 0u ) || ( div_num == 0u ) || ( div_den == 0u ) ||
         ( adc_vref_mv == 0u ) || ( shunt_uohm == 0u ) ) {
      shunt_uohm  = NvParams_GetDefault( NV_TEC_CURRENT_SHUNT_UOHM );
      amp_gain    = NvParams_GetDefault( NV_TEC_CURRENT_AMP_GAIN );
      div_num     = NvParams_GetDefault( NV_TEC_CURRENT_DIVIDER_NUM );
      div_den     = NvParams_GetDefault( NV_TEC_CURRENT_DIVIDER_DEN );
      adc_vref_mv = NvParams_GetDefault( NV_TEC_CURRENT_ADC_VREF_MV );
    }
  }

  /* Vadc = code * Vref / 4095; undo the INA output divider, then divide by
   * (shunt resistance * amplifier gain). Keep integer arithmetic in uV. */
  adc_voltage_uV = ((uint64_t)adc_code * adc_vref_mv * 1000ULL) / 4095ULL;
  shunt_voltage_uV = (adc_voltage_uV * div_den) / div_num;
  current_mA = (shunt_voltage_uV * 1000ULL) /
               ((uint64_t)shunt_uohm * amp_gain);
  return (uint32_t)current_mA;
}

int32_t Studio_ADC1_ReadHotMilliCelsius(void)
{
  return NTC3950_AdcToMilliCelsius(Studio_ADC1_ReadHotRaw());
}

int32_t Studio_ADC1_ReadColdMilliCelsius(void)
{
  /* Cold-side temperature source switch (NVRAM NV_TEC_COLD_SOURCE):
   *   0 = TEC_COLD ADC on PA4 (default), 1 = SDM845 host-reported temp. */
  if ( NvParams_Get( NV_TEC_COLD_SOURCE ) != 0u ) {
    return TRES_I2C_Slave_GetSdm845TempMilliC();
  }
  return NTC3950_AdcToMilliCelsius(Studio_ADC1_ReadColdRaw());
}

/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
