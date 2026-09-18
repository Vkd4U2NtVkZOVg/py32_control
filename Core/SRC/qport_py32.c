/*
 * qport_py32.c - Project-specific runtime port for the QuarkTS kernel.
 *
 * Registers the bare-metal critical-section (global interrupt enable/disable)
 * hooks used by the kernel. The disabler returns the previous PRIMASK so the
 * restorer can bring interrupts back to exactly the state they were in before
 * the critical section was entered (handles the "was IRQs already off?" case).
 */
#include "py32f0xx.h"
#include "QuarkTS.h"

/*----------------------------------------------------------------------------*/
static qUINT32_t qport_Disable_IRQs( void )
{
    qUINT32_t savedMask = __get_PRIMASK();

    __disable_irq();
    return savedMask;
}

/*----------------------------------------------------------------------------*/
static void qport_Restore_IRQs( qUINT32_t savedMask )
{
    __set_PRIMASK( savedMask );
}

/*----------------------------------------------------------------------------*/
void QPort_Setup( void )
{
    (void)qCritical_SetInterruptsED( qport_Restore_IRQs, qport_Disable_IRQs );
}