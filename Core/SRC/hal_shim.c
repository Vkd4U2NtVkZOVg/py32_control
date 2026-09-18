/**
  ******************************************************************************
  * @file    hal_shim.c
  * @brief   Minimal HAL time-base + GPIO shims for the LL-only build.
  *
  * This project intentionally builds only the PY32F030 Low-Layer (LL) drivers.
  * A few existing modules (the QuarkTS FSM demo in fsm_demo.c and main.c)
  * still call a handful of HAL entry points. Rather than pulling the whole HAL
  * driver tree into the build, these thin wrappers provide exactly those
  * entry points on top of the LL driver.
  *
  * Time base: HAL_IncTick() is called from SysTick_Handler() (py32f030_it.c)
  * at the 1 kHz system tick configured by Studio_RCC_Init() -> LL_Init1msTick().
  ******************************************************************************
  */
#include "main.h"
#include "py32f0xx_ll_gpio.h"

/*----------------------------------------------------------------------------*/
static volatile uint32_t uwTickCounter = 0U;

void HAL_IncTick(void)
{
    uwTickCounter++;
}

uint32_t HAL_GetTick(void)
{
    return uwTickCounter;
}

/*----------------------------------------------------------------------------*/
/* GPIO helpers used by the FSM demo (PF3 LED on instance 0).                 */
/*----------------------------------------------------------------------------*/
void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState)
{
    if (GPIO_PIN_SET == PinState) {
        LL_GPIO_SetOutputPin(GPIOx, GPIO_Pin);
    } else {
        LL_GPIO_ResetOutputPin(GPIOx, GPIO_Pin);
    }
}

void HAL_GPIO_TogglePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    LL_GPIO_TogglePin(GPIOx, GPIO_Pin);
}