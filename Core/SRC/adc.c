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
/* Factory-calibration attempt policy. Each attempt runs the 5-sample
 * median-of-5 sequence; an attempt is rejected when the hardware reports
 * CALFAIL or the calibration never completes. The sequence is retried a few
 * times before the ADC is declared unusable - see s_calib_failed. */
#define ADC_CAL_ATTEMPTS              ( 3 )
/* Bounded wait for "calibration ongoing" to clear. The 64-cycle conversion is
 * ~8 us at the 8 MHz ADC clock, so this is orders of magnitude of margin and
 * only ever reached if the peripheral is not responding at all. */
#define ADC_CAL_SPIN_LIMIT            ( 200000U )

/* Bounded wait for EOC on a single conversion, in poll passes.
 *
 * Sized from the hardware, not guessed: a conversion is 239.5 sampling +
 * 12.5 conversion clocks at the 8 MHz ADC clock, i.e. ~32 us. One pass of the
 * loop below (read ISR, decrement, compare, branch) is ~8-12 cycles at
 * 32 MHz, i.e. ~0.3 us - so the worst case needs roughly 100 passes and this
 * bound leaves a ~100x margin.
 *
 * The upper bound matters as much as the margin: adc1_convert() is called
 * three times per TEC control cycle (hot, cold, current), and a dead EOC
 * blocks the caller for the whole timeout. The previous 1000000 was
 * ~100-300 ms per channel - up to ~0.9 s of a 1 s control period spent
 * blocked, with the overtemp/overcurrent/fan-stall guards not running. At
 * 10000 the worst case is a few milliseconds, which the 1 s period absorbs. */
#define ADC_EOC_SPIN_LIMIT            ( 10000U )
/* USER CODE END Define */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN Macro */

/* USER CODE END Macro */

/* Public variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Private */

/* Set when the factory calibration could not be completed. The TEC control
 * task derives its over-temperature, over-current and dew-point guards from
 * this ADC, so an uncalibrated converter is a fail-safe condition, not a
 * warning: every temperature getter reports "no data" (INT32_MIN) and the
 * current getter reports an invalid sample, which routes the control loop
 * into its existing sensor-fault paths (TEC off, DAC parked, fan full,
 * TEC_ALARM_SENSOR) and is visible to the host over the I2C status block.
 * Uncalibrated readings would otherwise be silent: the guards would compare
 * against wrong temperatures and still look healthy. */
static uint8_t s_calib_failed;

/* USER CODE END Private */

/* Private function prototypes -----------------------------------------------*/
static void Studio_ADC1_Calibrate(void);
static uint8_t Studio_ADC1_CalibrateOnce(void);
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

/* One calibration attempt: 5 samples, median written back. Returns 1 when the
 * sequence completed without CALFAIL, 0 when the hardware reported a failure
 * or the calibration never finished. The vendor sequence (median of 5) is kept
 * as-is; only the failure detection is added. */
static uint8_t Studio_ADC1_CalibrateOnce(void)
{
  uint32_t ADC_CALRR1_Buff[5];
  int32_t ADC_CALRR1_BuffInt[5];

  for (int i = 0; i < 5; i++)
  {
    uint32_t spin = ADC_CAL_SPIN_LIMIT;

    LL_ADC_StartCalibration(ADC1);

    while (LL_ADC_IsCalibrationOnGoing(ADC1) != 0)
    {
      if (spin-- == 0u)
      {
        return 0u;                    /* calibration never completed */
      }
    }

    if (LL_ADC_GetCalibrationStatus(ADC1) == LL_ADC_CAL_STATUS_FAIL)
    {
      LL_ADC_ClearCalibrationStatus(ADC1);
      return 0u;                      /* hardware refused this calibration */
    }

    ADC_CALRR1_Buff[i] = *((__IO uint32_t *)(0x40012448));
    ADC_CALRR1_BuffInt[i] = (int32_t)(ADC_CALRR1_Buff[i] << 9);
  }

  Studio_ADC1_QuickSort(ADC_CALRR1_BuffInt, 5);
  *((__IO uint32_t *)(0x40012450)) = (ADC_CALRR1_BuffInt[2] >> 9);
  *((__IO uint32_t *)(0x40012454)) = *((__IO uint32_t *)(0x4001244C));
  *((__IO uint32_t *)(0x40012444)) |= 0x8000;
  /* Delay between ADC calibration end and ADC enable: minimum 4 ADC Clock cycles */
  LL_mDelay(1);
  return 1u;
}

/**
  * @brief  ADC calibration function
  * @param  None
  * @retval None
  */
static void Studio_ADC1_Calibrate(void)
{
  __IO uint32_t backup_setting_adc_dma_transfer = 0;
  int attempt;

  s_calib_failed = 0u;

  if (LL_ADC_IsEnabled(ADC1) != 0)
  {
    return;                           /* already running: nothing to calibrate */
  }

  for (attempt = 0; attempt < ADC_CAL_ATTEMPTS; attempt++)
  {
    uint8_t ok;

#if (defined(DMA) || defined(DMA1))
    /* Disable ADC DMA transfer during calibration */
    backup_setting_adc_dma_transfer = LL_ADC_REG_GetDMATransfer(ADC1);
    LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_NONE);
#endif
    ok = Studio_ADC1_CalibrateOnce();
#if (defined(DMA) || defined(DMA1))
    /* Restore ADC DMA configuration */
    LL_ADC_REG_SetDMATransfer(ADC1, backup_setting_adc_dma_transfer);
#endif
    if (ok != 0u)
    {
      return;
    }
  }

  s_calib_failed = 1u;
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
/* Checked variant: 1 on success; 0 when EOC never arrives (clock/peripheral */
/* fault), with *out set to 0. Callers that cannot treat 0 as "no data" -    */
/* the current path, where 0 mA is a legitimate TEC-off reading - must use   */
/* the validity flag instead of the raw value.                               */
static uint16_t adc1_convert(uint32_t channel, uint8_t *ok)
{
  uint16_t value = 0U;
  uint32_t timeout = ADC_EOC_SPIN_LIMIT;   /* abort if EOC never arrives */

  /* Clear all channels */
  WRITE_REG(ADC1->CHSELR, 0U);
  /* Select the wanted channel */
  LL_ADC_REG_SetSequencerChannels(ADC1, channel);

  LL_ADC_Enable(ADC1);                             /* ADC must remain enabled; no disable */
  LL_mDelay(1);                                    /* ADC stabilization, min 8 ADC clocks */

  /* Drop any EOC still latched by a conversion a previous call abandoned on
   * timeout: that conversion completed AFTER we stopped waiting, so its flag
   * is set but its data belongs to a different channel. StartConversion()
   * does not clear EOC (hardware keeps it latched), so without this the loop
   * below would exit on the first pass and read the other channel's sample -
   * a stale reading that looks perfectly valid. */
  LL_ADC_ClearFlag_EOC(ADC1);
  LL_ADC_REG_StartConversion(ADC1);
  while (LL_ADC_IsActiveFlag_EOC(ADC1) == 0U) {
    if ( timeout-- == 0U ) {
      /* Conversion never completed (clock/peripheral fault). */
      WRITE_REG(ADC1->CHSELR, 0U);
      *ok = 0U;
      return 0U;
    }
  }
  LL_ADC_ClearFlag_EOC(ADC1);
  value = LL_ADC_REG_ReadConversionData12(ADC1);

  /* Clear all channels */
  WRITE_REG(ADC1->CHSELR, 0U);

  *ok = 1U;
  return value;
}

uint16_t Studio_ADC1_ReadChannel(uint32_t channel)
{
  uint8_t ok;
  uint16_t value = adc1_convert(channel, &ok);
  /* 0 on timeout: the hot/cold NTC callers convert it to an out-of-window
   * temperature and fail safe. The current path must NOT rely on this - a
   * genuine 0 mA (TEC off) and a timed-out sample are the same code there,
   * so Studio_ADC1_ReadCurrentMilliamp() reports validity separately. */
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
  uint8_t ok;
  return adc1_convert(LL_ADC_CHANNEL_6, &ok);
}

uint8_t Studio_ADC1_ReadCurrentMilliamp(uint32_t *out_ma)
{
  /* Uncalibrated converter: the sample is meaningless. Report the same
   * invalid-sample shape as an ADC timeout (0 = "no valid sample", never a
   * silent 0 mA) so the overcurrent guard runs its fail-safe path instead of
   * comparing a bogus current against the danger threshold. */
  if ( s_calib_failed != 0u ) {
    *out_ma = 0u;
    return 0U;
  }
  /* Current-sense chain calibration from NVRAM (INA181A2 + divider). Values
   * are snapshotted once; changing NVRAM takes effect after reboot. Zero OR
   * out-of-envelope values refetch and, if still bad, fall back to the
   * factory defaults: extreme non-zero magnitudes are physically meaningless
   * AND overflow the intermediate uV products (uint64) - a divider
   * denominator near 2^32 pushes shunt_voltage_uV past 2^64, and two near-
   * 2^32 factors overflow the shunt*gain denominator. Envelopes fit any
   * sane chain: shunt 0.1 mOhm..1 Ohm, gain 1..1000, divider 1..1e6, Vref
   * 1 mV..5 V. Bounded this way the worst-case product stays below 2^53. */
  static uint32_t shunt_uohm = 0u;
  static uint32_t amp_gain   = 0u;
  static uint32_t div_num    = 0u;
  static uint32_t div_den    = 0u; /* zero means not loaded, not unity gain */
  static uint32_t adc_vref_mv = 0u;
  uint16_t adc_code;
  uint8_t  ok;
  uint64_t adc_voltage_uV;
  uint64_t shunt_voltage_uV;
  uint64_t current_mA;

  if ( ( shunt_uohm < 100u ) || ( shunt_uohm > 1000000u ) ) {
    shunt_uohm = NvParams_Get( NV_TEC_CURRENT_SHUNT_UOHM );
  }
  if ( ( amp_gain < 1u ) || ( amp_gain > 1000u ) ) {
    amp_gain = NvParams_Get( NV_TEC_CURRENT_AMP_GAIN );
  }
  if ( ( div_num < 1u ) || ( div_num > 1000000u ) ) {
    div_num = NvParams_Get( NV_TEC_CURRENT_DIVIDER_NUM );
  }
  if ( ( div_den < 1u ) || ( div_den > 1000000u ) ) {
    div_den = NvParams_Get( NV_TEC_CURRENT_DIVIDER_DEN );
  }
  if ( ( adc_vref_mv < 1u ) || ( adc_vref_mv > 5000u ) ) {
    adc_vref_mv = NvParams_Get( NV_TEC_CURRENT_ADC_VREF_MV );
  }
  if ( ( shunt_uohm < 100u ) || ( shunt_uohm > 1000000u ) ||
       ( amp_gain < 1u ) || ( amp_gain > 1000u ) ||
       ( div_num < 1u ) || ( div_num > 1000000u ) ||
       ( div_den < 1u ) || ( div_den > 1000000u ) ||
       ( adc_vref_mv < 1u ) || ( adc_vref_mv > 5000u ) ) {
    shunt_uohm  = NvParams_GetDefault( NV_TEC_CURRENT_SHUNT_UOHM );
    amp_gain    = NvParams_GetDefault( NV_TEC_CURRENT_AMP_GAIN );
    div_num     = NvParams_GetDefault( NV_TEC_CURRENT_DIVIDER_NUM );
    div_den     = NvParams_GetDefault( NV_TEC_CURRENT_DIVIDER_DEN );
    adc_vref_mv = NvParams_GetDefault( NV_TEC_CURRENT_ADC_VREF_MV );
  }

  /* A timed-out conversion reads as code 0 - indistinguishable at this
   * layer from a genuine 0 mA (TEC off) - so sample validity is reported
   * explicitly instead of letting 0 stand in for "no data" (a bare 0 here
   * would silently blind the caller's overcurrent guard). */
  adc_code = adc1_convert(LL_ADC_CHANNEL_6, &ok);
  if ( ok == 0U ) {
    *out_ma = 0U;
    return 0U;
  }

  /* Vadc = code * Vref / 4095; undo the INA output divider, then divide by
   * (shunt resistance * amplifier gain). Keep integer arithmetic in uV. */
  adc_voltage_uV = ((uint64_t)adc_code * adc_vref_mv * 1000ULL) / 4095ULL;
  shunt_voltage_uV = (adc_voltage_uV * div_den) / div_num;
  current_mA = (shunt_voltage_uV * 1000ULL) /
               ((uint64_t)shunt_uohm * amp_gain);
  if ( current_mA > UINT32_MAX ) {
    *out_ma = UINT32_MAX;
    return 0U; /* unrepresentable calibration result is never zero current */
  }
  *out_ma = (uint32_t)current_mA;
  return 1U;
}

int32_t Studio_ADC1_ReadHotMilliCelsius(void)
{
  if ( s_calib_failed != 0u ) {
    return INT32_MIN;   /* uncalibrated: report no data, not a wrong temperature */
  }
  return NTC3950_AdcToMilliCelsius(Studio_ADC1_ReadHotRaw());
}

int32_t Studio_ADC1_ReadColdMilliCelsius(void)
{
  /* Cold-side temperature source switch (NVRAM NV_TEC_COLD_SOURCE):
   *   0 = TEC_COLD ADC on PA4 (default), 1 = SDM845 host-reported temp.
   * Host has not written a temp yet (s_temp_valid == 0) -> return the
   * "no data" sentinel (same convention as the AHT20 dew point), so the
   * tec_control plausibility window rejects it and the loop falls back
   * to nominal instead of PID-driving full cooling against a stale 0 C. */
  if ( NvParams_Get( NV_TEC_COLD_SOURCE ) != 0u ) {
    if ( TRES_I2C_Slave_GetSdm845TempValid() == 0u ) {
      return INT32_MIN;
    }
    return TRES_I2C_Slave_GetSdm845TempMilliC();
  }
  if ( s_calib_failed != 0u ) {
    return INT32_MIN;   /* ADC path needs the calibration; the SoC path does not */
  }
  return NTC3950_AdcToMilliCelsius(Studio_ADC1_ReadColdRaw());
}

/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

