/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    py32f0xx_hal_conf.h
  * @brief   HAL configuration file.
  *
  * Project-owned copy of the HAL configuration header. It is placed in
  * "Core/INC" (first on the include path) so the PY32 HAL driver uses exactly
  * the module set enabled below without editing the driver sources.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __PY32F0xx_HAL_CONF_H
#define __PY32F0xx_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------- Modules enabled ----------------------------- */
#define HAL_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_EXTI_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_ADC_MODULE_ENABLED
#define HAL_I2C_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_USART_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_IRDA_MODULE_ENABLED
#define HAL_CRC_MODULE_ENABLED
#define HAL_RTC_MODULE_ENABLED
#define HAL_COMP_MODULE_ENABLED
#define HAL_IWDG_MODULE_ENABLED
#define HAL_WWDG_MODULE_ENABLED
#define HAL_LPTIM_MODULE_ENABLED
#define HAL_LED_MODULE_ENABLED

/* --------------------------- Module includes ----------------------------- */
#ifdef HAL_RCC_MODULE_ENABLED
  #include "py32f0xx_hal_rcc.h"
#endif /* HAL_RCC_MODULE_ENABLED */

#ifdef HAL_GPIO_MODULE_ENABLED
  #include "py32f0xx_hal_gpio.h"
#endif /* HAL_GPIO_MODULE_ENABLED */

#ifdef HAL_EXTI_MODULE_ENABLED
  #include "py32f0xx_hal_exti.h"
#endif /* HAL_EXTI_MODULE_ENABLED */

#ifdef HAL_CORTEX_MODULE_ENABLED
  #include "py32f0xx_hal_cortex.h"
#endif /* HAL_CORTEX_MODULE_ENABLED */

#ifdef HAL_DMA_MODULE_ENABLED
  #include "py32f0xx_hal_dma.h"
#endif /* HAL_DMA_MODULE_ENABLED */

#ifdef HAL_ADC_MODULE_ENABLED
  #include "py32f0xx_hal_adc.h"
#endif /* HAL_ADC_MODULE_ENABLED */

#ifdef HAL_FLASH_MODULE_ENABLED
  #include "py32f0xx_hal_flash.h"
#endif /* HAL_FLASH_MODULE_ENABLED */

#ifdef HAL_PWR_MODULE_ENABLED
  #include "py32f0xx_hal_pwr.h"
#endif /* HAL_PWR_MODULE_ENABLED */

#ifdef HAL_I2C_MODULE_ENABLED
  #include "py32f0xx_hal_i2c.h"
#endif /* HAL_I2C_MODULE_ENABLED */

#ifdef HAL_SPI_MODULE_ENABLED
  #include "py32f0xx_hal_spi.h"
#endif /* HAL_SPI_MODULE_ENABLED */

#ifdef HAL_TIM_MODULE_ENABLED
  #include "py32f0xx_hal_tim.h"
#endif /* HAL_TIM_MODULE_ENABLED */

#ifdef HAL_USART_MODULE_ENABLED
  #include "py32f0xx_hal_usart.h"
#endif /* HAL_USART_MODULE_ENABLED */

#ifdef HAL_UART_MODULE_ENABLED
  #include "py32f0xx_hal_uart.h"
#endif /* HAL_UART_MODULE_ENABLED */

#ifdef HAL_IRDA_MODULE_ENABLED
  #include "py32f0xx_hal_irda.h"
#endif /* HAL_IRDA_MODULE_ENABLED */

#ifdef HAL_CRC_MODULE_ENABLED
  #include "py32f0xx_hal_crc.h"
#endif /* HAL_CRC_MODULE_ENABLED */

#ifdef HAL_RTC_MODULE_ENABLED
  #include "py32f0xx_hal_rtc.h"
#endif /* HAL_RTC_MODULE_ENABLED */

#ifdef HAL_COMP_MODULE_ENABLED
  #include "py32f0xx_hal_comp.h"
#endif /* HAL_COMP_MODULE_ENABLED */

#ifdef HAL_IWDG_MODULE_ENABLED
  #include "py32f0xx_hal_iwdg.h"
#endif /* HAL_IWDG_MODULE_ENABLED */

#ifdef HAL_WWDG_MODULE_ENABLED
  #include "py32f0xx_hal_wwdg.h"
#endif /* HAL_WWDG_MODULE_ENABLED */

#ifdef HAL_LPTIM_MODULE_ENABLED
  #include "py32f0xx_hal_lptim.h"
#endif /* HAL_LPTIM_MODULE_ENABLED */

#ifdef HAL_LED_MODULE_ENABLED
  #include "py32f0xx_hal_led.h"
#endif /* HAL_LED_MODULE_ENABLED */

/* ---------------------------- Assert --------------------------------------------------- */
/* Time base tick interrupt priority (used by HAL_InitTick / HAL_Delay).      */
#define TICK_INT_PRIORITY  0U

#ifdef  USE_FULL_ASSERT
/**
  * @brief  The assert_param macro is used for function's parameters check.
  * @param  expr: If expr is false, it calls assert_failed function
  *         which reports the name of the source file and the source
  *         line number of the call that failed.
  */
  #define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
/* Exported functions ------------------------------------------------------- */
void assert_failed(uint8_t *file, uint32_t line);
#else
  #define assert_param(expr) ((void)0U)
#endif /* USE_FULL_ASSERT */

#ifdef __cplusplus
}
#endif

#endif /* __PY32F0xx_HAL_CONF_H */