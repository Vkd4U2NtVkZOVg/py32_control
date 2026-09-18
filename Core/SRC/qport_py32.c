/*
 * qport_py32.c - Project-specific runtime port for the QuarkTS kernel.
 *
 * Responsibilities:
 *   1. Register the bare-metal critical-section (global interrupt enable/
 *      disable) hooks used by the kernel. The disabler returns the previous
 *      PRIMASK so the restorer can bring interrupts back to exactly the state
 *      they were in before the critical section was entered.
 *   2. Provide the millisecond time base for the scheduler. LL_Init1msTick()
 *      (called from Studio_RCC_Init) only starts the SysTick timer; it does
 *      NOT enable the SysTick interrupt. Here we enable that interrupt and
 *      supply a simple counter that SysTick_Handler advances once per ms, so
 *      QOS_Tick_Get() can be handed to qOS_Setup() as the tick source.
 */
#include "py32f0xx.h"
#include "QuarkTS.h"

/*----------------------------------------------------------------------------*/
/* Critical-section handlers                                                  */
/*----------------------------------------------------------------------------*/
static qUINT32_t qport_Disable_IRQs( void )
{
    qUINT32_t savedMask = __get_PRIMASK();

    __disable_irq();
    return savedMask;
}

static void qport_Restore_IRQs( qUINT32_t savedMask )
{
    __set_PRIMASK( savedMask );
}

/*----------------------------------------------------------------------------*/
/* Millisecond time base                                                      */
/*----------------------------------------------------------------------------*/
static volatile qUINT32_t qport_tick = 0U;

/* Incremented by SysTick_Handler() once per millisecond. */
void QOS_Tick_Inc( void )
{
    qport_tick++;
}

/* Tick getter, compatible with qGetTickFcn_t (void -> uint32_t). */
qUINT32_t QOS_Tick_Get( void )
{
    return qport_tick;
}

/*----------------------------------------------------------------------------*/
void QPort_Setup( void )
{
    (void)qCritical_SetInterruptsED( qport_Restore_IRQs, qport_Disable_IRQs );

    /* Enable the SysTick interrupt so QOS_Tick_Inc() runs at 1 kHz. */
    NVIC_SetPriority( SysTick_IRQn, 0U );
    SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;
}