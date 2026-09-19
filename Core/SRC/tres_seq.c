/**
  ******************************************************************************
  * @file    tres_seq.c
  * @brief   TRES (STM32H7) power/boot sequencer state machine (QuarkTS).
  *
  * The machine is polled every 10 ms. QuarkTS invokes the current state
  * callback on every task cycle (Signal == QSM_SIGNAL_NONE when nothing is
  * queued), so tick-based supervision (BOOT_WAIT_NORMAL, RUNNING host-lost,
  * optional FAULT retry) works without extra wake-up signals. Fixed one-shot
  * sequencing delays use const timeout-spec tables, exactly like the
  * s_fastTO[] pattern of fsm_demo.c.
  *
  * retry counting: incremented on every failed cold boot (SOM_BOOTED missing
  * within TRES_T_HOSTBOOT_MS) or host CMD_REPORT_FAIL; cleared whenever the
  * chain confirms healthy (SOM_BOOTED edge into RUNNING). Reaching
  * TRES_N_MAX_RETRIES latches FAULT; only a host CMD_PWR_ON clears it.
  ******************************************************************************
  */
#include "main.h"
#include "QuarkTS.h"
#include "tres_seq.h"
#include "qport_py32.h"

/*============================================================================*/
/* Internal signals (user signal range 0 .. QSM_SIGNAL_RANGE_MAX)             */
/*============================================================================*/
#define SIG_PWR_ON_NORMAL   ((qSM_SigId_t)1u)
#define SIG_PWR_ON_DFU      ((qSM_SigId_t)2u)
#define SIG_POWER_OFF       ((qSM_SigId_t)3u)
#define SIG_RESET           ((qSM_SigId_t)4u)
#define SIG_REBOOT_NORMAL   ((qSM_SigId_t)5u)
#define SIG_REPORT_FAIL     ((qSM_SigId_t)6u)
#define SIG_SEQ_TIMEOUT     (QSM_SIGNAL_TIMEOUT(0))  /**< one-shot sequencing  */

/*============================================================================*/
/* Per-machine runtime data (h->StateData of every state callback)            */
/*============================================================================*/
typedef struct {
    uint8_t   mode;         /**< TRES_Mode_t of the boot in progress           */
    uint8_t   pendingNext;  /**< 0xFF = none, else TRES_Mode_t to auto-boot    */
                            /**< after SHUTDOWN completes (reboot paths)      */
    uint8_t   retries;
    uint8_t   hostReady;    /**< live SOM_BOOTED level                         */
    uint8_t   faultLatched;
    uint8_t   stateId;      /**< TRES_State_t mirror, updated on each entry    */
    uint8_t   somPrev;      /**< previous SOM_BOOTED level (edge detection)    */
    uint8_t   somRise;      /**< consumed by BOOT_WAIT_NORMAL                  */
    uint8_t   somFall;      /**< consumed by RUNNING (host-lost policy)        */
    qUINT32_t entryTick;    /**< tick captured at state entry                  */
    qUINT32_t somFallTick;  /**< tick of the last host-lost edge               */
} TRES_Data_t;

static qSM_State_t stOff, stPwrUp, stBootSetup, stBootWaitN, stBootWaitD,
                   stRunning, stDfuActive, stPulseReset, stShutdown, stFault;

typedef struct {
    qTask_t            task;
    qSM_t              machine;
    qSM_TimeoutSpec_t  timeoutSpec;
    qQueue_t           sigQueue;
    qSM_Signal_t       sigStack[ 8 ];
    TRES_Data_t        data;
} TRES_Seq_t;

static TRES_Seq_t s_seq;

/*============================================================================*/
/* Pin primitives (the ONLY code allowed to touch the TRES control lines)     */
/*============================================================================*/
static void Pins_AllOff( void )
{
    LL_GPIO_ResetOutputPin( GPIOB, TRES_PWR_EN_Pin | TRES_BOOT0_Pin );
    LL_GPIO_ResetOutputPin( GPIOA, TRES_RST_Pin | TRES_VDD33USB_CTRL_Pin );
}

static void Pins_PwrOn( void )
{
    LL_GPIO_SetOutputPin( GPIOB, TRES_PWR_EN_Pin );
}

static void Pins_PwrOff( void )
{
    LL_GPIO_ResetOutputPin( GPIOB, TRES_PWR_EN_Pin );
}

static void Pins_RstAssert( void )
{
    LL_GPIO_ResetOutputPin( GPIOA, TRES_RST_Pin );
}

static void Pins_RstRelease( void )
{
    LL_GPIO_SetOutputPin( GPIOA, TRES_RST_Pin );
}

static void Pins_Boot0( uint8_t on )
{
    if ( on != 0u ) { LL_GPIO_SetOutputPin( GPIOB, TRES_BOOT0_Pin ); }
    else           { LL_GPIO_ResetOutputPin( GPIOB, TRES_BOOT0_Pin ); }
}

static void Pins_VddUsb( uint8_t on )
{
    if ( on != 0u ) { LL_GPIO_SetOutputPin( GPIOA, TRES_VDD33USB_CTRL_Pin ); }
    else           { LL_GPIO_ResetOutputPin( GPIOA, TRES_VDD33USB_CTRL_Pin ); }
}

static uint8_t Pins_SomBooted( void )
{
    return ( LL_GPIO_IsInputPinSet( GPIOB, SOM_BOOTED_Pin ) != 0u ) ? 1u : 0u;
}

/*============================================================================*/
/* SOM_BOOTED sampler: edge detection + host_ready tracking. Called from the  */
/* states where the level is meaningful (BOOT_WAIT_NORMAL, RUNNING).          */
/*============================================================================*/
static void Som_Sample( TRES_Data_t *d )
{
    uint8_t now = Pins_SomBooted();

    d->somRise = 0u;
    d->somFall = 0u;
    if ( now != d->somPrev ) {
        if ( now != 0u ) {
            d->somRise = 1u;
        } else {
            d->somFall    = 1u;
            d->somFallTick = QOS_Tick_Get();
        }
        d->somPrev   = now;
        d->hostReady = now;
    }
}

static void State_RetryOrFail( qSM_Handler_t h, TRES_Data_t *d )
{
    d->retries++;
    if ( d->retries >= TRES_N_MAX_RETRIES ) {
        h->NextState = &stFault;
    } else {
        d->pendingNext = d->mode;      /* power-cycle into the same mode */
        h->NextState   = &stShutdown;
    }
}

/*============================================================================*/
/* State callbacks (flat hierarchy, all states run on every 10 ms cycle)      */
/*============================================================================*/
static qSM_Status_t st_Off_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            Pins_AllOff();
            d->stateId = (uint8_t)TRES_ST_OFF;
            break;
        case SIG_PWR_ON_NORMAL:
            d->mode    = TRES_MODE_NORMAL;
            d->retries = 0u;
            h->NextState = &stPwrUp;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_PWR_ON_DFU:
            d->mode    = TRES_MODE_DFU;
            d->retries = 0u;
            h->NextState = &stPwrUp;
            return qSM_STATUS_SIGNAL_HANDLED;
        case QSM_SIGNAL_NONE:
            /* automatic re-boot requested by a completed SHUTDOWN            */
            if ( d->pendingNext != 0xFFu ) {
                d->mode        = d->pendingNext;
                d->pendingNext = 0xFFu;
                h->NextState   = &stPwrUp;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            break;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_PwrUp_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId   = (uint8_t)TRES_ST_PWR_UP;
            Pins_PwrOn();              /* RST stays asserted during ramp     */
            break;
        case SIG_SEQ_TIMEOUT:
            h->NextState = &stBootSetup;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_POWER_OFF:            /* host aborts mid-sequence           */
            d->pendingNext = 0xFFu;
            h->NextState   = &stShutdown;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_BootSetup_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_ST_BOOT_SETUP;
            Pins_Boot0( ( d->mode == TRES_MODE_DFU ) ? 1u : 0u );
            Pins_VddUsb( ( d->mode == TRES_MODE_DFU ) ? 1u :
                         (uint8_t)TRES_NORMAL_VDD33USB_ON );
            break;
        case SIG_SEQ_TIMEOUT:
            h->NextState = ( d->mode == TRES_MODE_DFU ) ? &stBootWaitD
                                                        : &stBootWaitN;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_POWER_OFF:
            d->pendingNext = 0xFFu;
            h->NextState   = &stShutdown;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_BootWaitNormal_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    if ( QSM_SIGNAL_ENTRY == h->Signal ) {
        d->stateId   = (uint8_t)TRES_ST_BOOT_WAIT_NORMAL;
        d->entryTick = QOS_Tick_Get();
        Pins_RstRelease();             /* BOOT0 is latched by the H7 here    */
        /* capture current level so a pre-asserted host (pulse-reset path)   */
        /* does not look like a fresh edge                                   */
        d->somPrev = Pins_SomBooted();
        d->hostReady = d->somPrev;
        d->somRise = 0u;
        d->somFall = 0u;
        return qSM_STATUS_EXIT_SUCCESS;
    }

    switch ( h->Signal ) {
        case SIG_POWER_OFF:
            d->pendingNext = 0xFFu;
            h->NextState   = &stShutdown;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_REPORT_FAIL:
            State_RetryOrFail( h, d );
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }

    /* periodic supervision (runs on every cycle)                             */
    Som_Sample( d );
    if ( d->somRise != 0u ) {
        /* TRES booted and started the host: whole chain confirmed            */
        d->retries   = 0u;
        h->NextState = &stRunning;
        return qSM_STATUS_SIGNAL_HANDLED;
    }
    if ( d->somPrev != 0u ) {
        /* reset/reboot with a live host: no fresh edge will come, grace only */
        if ( ( QOS_Tick_Get() - d->entryTick ) >= TRES_T_APPBOOT_MS ) {
            d->retries   = 0u;
            h->NextState = &stRunning;
            return qSM_STATUS_SIGNAL_HANDLED;
        }
    } else if ( ( QOS_Tick_Get() - d->entryTick ) >= TRES_T_HOSTBOOT_MS ) {
        /* cold boot failed: nobody can report it, supervise locally          */
        State_RetryOrFail( h, d );
        return qSM_STATUS_SIGNAL_HANDLED;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_BootWaitDfu_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_ST_BOOT_WAIT_DFU;
            Pins_RstRelease();         /* BOOT0=1 latched: system DFU starts */
            break;
        case SIG_SEQ_TIMEOUT:
            h->NextState = &stDfuActive;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_POWER_OFF:
            d->pendingNext = 0xFFu;
            h->NextState   = &stShutdown;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_Running_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    if ( QSM_SIGNAL_ENTRY == h->Signal ) {
        d->stateId = (uint8_t)TRES_ST_RUNNING;
        d->hostReady = Pins_SomBooted();
        d->somPrev   = d->hostReady;
        d->somRise = 0u;
        d->somFall = 0u;
        return qSM_STATUS_EXIT_SUCCESS;
    }

    switch ( h->Signal ) {
        case SIG_RESET:
            h->NextState = &stPulseReset;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_POWER_OFF:
            d->pendingNext = 0xFFu;
            h->NextState   = &stShutdown;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_REBOOT_NORMAL:
            d->pendingNext = (uint8_t)TRES_MODE_NORMAL;
            h->NextState   = &stShutdown;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_PWR_ON_DFU:
            d->pendingNext = (uint8_t)TRES_MODE_DFU;
            h->NextState   = &stShutdown;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_REPORT_FAIL:
            State_RetryOrFail( h, d );
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }

#if ( TRES_REBOOT_CHAIN_ON_HOST_LOST > 0 )
    /* optional supervision: host disappeared and never came back            */
    Som_Sample( d );
    if ( ( d->somFall != 0u ) && ( d->hostReady == 0u ) &&
         ( ( QOS_Tick_Get() - d->somFallTick ) >= TRES_T_HOSTLOST_MS ) ) {
        d->pendingNext = (uint8_t)TRES_MODE_NORMAL;
        h->NextState   = &stShutdown;
        return qSM_STATUS_SIGNAL_HANDLED;
    }
#else
    Som_Sample( d );                   /* keep host_ready in STATUS fresh    */
#endif
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_DfuActive_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_ST_DFU_ACTIVE;
            /* VDD33USB must stay on; host drives the DFU session             */
            break;
        case SIG_RESET:
            h->NextState = &stPulseReset;   /* BOOT0 still 1 -> DFU again      */
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_POWER_OFF:
            d->pendingNext = 0xFFu;
            h->NextState   = &stShutdown;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_REBOOT_NORMAL:
            d->pendingNext = (uint8_t)TRES_MODE_NORMAL;
            h->NextState   = &stShutdown;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_REPORT_FAIL:
            State_RetryOrFail( h, d );
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_PulseReset_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_ST_PULSE_RESET;
            Pins_RstAssert();          /* BOOT0 level unchanged              */
            break;
        case SIG_SEQ_TIMEOUT:
            h->NextState = ( d->mode == TRES_MODE_DFU ) ? &stBootWaitD
                                                        : &stBootWaitN;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_Shutdown_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_ST_SHUTDOWN;
            /* fixed order: kill the app first, then rails, then power        */
            Pins_RstAssert();
            Pins_VddUsb( 0u );
            Pins_PwrOff();
            break;
        case SIG_SEQ_TIMEOUT:          /* discharge guard elapsed            */
            h->NextState = &stOff;     /* OFF re-boots if pendingNext set    */
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;                     /* busy: all commands rejected here   */
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_Fault_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_ST_FAULT;
            d->faultLatched = 1u;
            d->entryTick    = QOS_Tick_Get();
            Pins_AllOff();
            break;
        case SIG_PWR_ON_NORMAL:        /* host explicitly retries            */
            d->faultLatched = 0u;
            d->retries      = 0u;
            d->mode         = TRES_MODE_NORMAL;
            h->NextState    = &stPwrUp;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_PWR_ON_DFU:
            d->faultLatched = 0u;
            d->retries      = 0u;
            d->mode         = TRES_MODE_DFU;
            h->NextState    = &stPwrUp;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_POWER_OFF:
            d->faultLatched = 0u;
            h->NextState    = &stOff;
            return qSM_STATUS_SIGNAL_HANDLED;
#if ( TRES_FAULT_RETRY_MS > 0 )
        case QSM_SIGNAL_NONE:
            /* optional self-healing for cold-boot failures with no host      */
            if ( ( QOS_Tick_Get() - d->entryTick ) >= TRES_FAULT_RETRY_MS ) {
                d->faultLatched = 0u;
                d->entryTick    = QOS_Tick_Get();
                if ( d->pendingNext == 0xFFu ) { d->pendingNext = d->mode; }
                h->NextState = &stShutdown;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            break;
#endif
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/*============================================================================*/
/* Const timeout specifications (flash-resident, one one-shot timer per state)*/
/*============================================================================*/
static const qSM_TimeoutStateDefinition_t s_toPwrUp[] = {
    { (qTime_t)( (float)TRES_T_RAIL_MS / 1000.0f ),
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toSetup[] = {
    { (qTime_t)( (float)TRES_T_SETUP_MS / 1000.0f ),
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toEnum[] = {
    { (qTime_t)( (float)TRES_T_ENUM_MS / 1000.0f ),
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toPulse[] = {
    { (qTime_t)( (float)TRES_T_RST_MS / 1000.0f ),
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toShutdown[] = {
    { (qTime_t)( (float)TRES_T_DISCHARGE_MS / 1000.0f ),
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};

/*============================================================================*/
/* Public request interface: command layer -> FSM signal queue                */
/*============================================================================*/
static void Seq_Send( qSM_SigId_t sig )
{
    (void)qStateMachine_SendSignal( &s_seq.machine, sig, NULL, qFalse );
}

void TRES_Seq_RequestPowerOn( TRES_Mode_t mode )
{
    Seq_Send( ( mode == TRES_MODE_DFU ) ? SIG_PWR_ON_DFU : SIG_PWR_ON_NORMAL );
}

void TRES_Seq_RequestPowerOff( void )      { Seq_Send( SIG_POWER_OFF ); }
void TRES_Seq_RequestReset( void )         { Seq_Send( SIG_RESET ); }
void TRES_Seq_RequestRebootNormal( void )  { Seq_Send( SIG_REBOOT_NORMAL ); }
void TRES_Seq_RequestFailReport( void )    { Seq_Send( SIG_REPORT_FAIL ); }

/*============================================================================*/
/* Status interface                                                           */
/*============================================================================*/
uint8_t TRES_Seq_GetState( void )       { return s_seq.data.stateId; }
uint8_t TRES_Seq_GetRetries( void )     { return s_seq.data.retries; }
uint8_t TRES_Seq_HostReady( void )      { return s_seq.data.hostReady; }
uint8_t TRES_Seq_FaultLatched( void )   { return s_seq.data.faultLatched; }

uint8_t TRES_Seq_IsBusy( void )
{
    return ( s_seq.data.stateId == (uint8_t)TRES_ST_SHUTDOWN ) ? 1u : 0u;
}

uint8_t TRES_Seq_GetPinsSnapshot( void )
{
    uint8_t v = 0u;

    if ( LL_GPIO_IsOutputPinSet( GPIOB, TRES_PWR_EN_Pin ) != 0u )         { v |= 0x01u; }
    if ( LL_GPIO_IsOutputPinSet( GPIOA, TRES_RST_Pin ) != 0u )            { v |= 0x02u; }
    if ( LL_GPIO_IsOutputPinSet( GPIOB, TRES_BOOT0_Pin ) != 0u )          { v |= 0x04u; }
    if ( LL_GPIO_IsOutputPinSet( GPIOA, TRES_VDD33USB_CTRL_Pin ) != 0u )  { v |= 0x08u; }
    if ( LL_GPIO_IsInputPinSet( GPIOB, SOM_BOOTED_Pin ) != 0u )           { v |= 0x10u; }
    return v;
}

/*============================================================================*/
/* Public: build the topology, register the task, inject the cold autoboot    */
/*============================================================================*/
void TRES_Seq_Init( void )
{
    s_seq.data.mode         = (uint8_t)TRES_MODE_NORMAL;
    s_seq.data.pendingNext  = 0xFFu;
    s_seq.data.retries      = 0u;
    s_seq.data.hostReady    = 0u;
    s_seq.data.faultLatched = 0u;
    s_seq.data.stateId      = (uint8_t)TRES_ST_OFF;
    s_seq.data.somPrev      = 0u;
    s_seq.data.somRise      = 0u;
    s_seq.data.somFall      = 0u;

    qStateMachine_Setup( &s_seq.machine, NULL, &stOff, NULL, NULL );

    qStateMachine_StateSubscribe( &s_seq.machine, &stOff,        NULL, st_Off_Cb,           NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stPwrUp,      NULL, st_PwrUp_Cb,         NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stBootSetup,  NULL, st_BootSetup_Cb,     NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stBootWaitN,  NULL, st_BootWaitNormal_Cb, NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stBootWaitD,  NULL, st_BootWaitDfu_Cb,   NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stRunning,    NULL, st_Running_Cb,       NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stDfuActive,  NULL, st_DfuActive_Cb,     NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stPulseReset, NULL, st_PulseReset_Cb,    NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stShutdown,   NULL, st_Shutdown_Cb,      NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stFault,      NULL, st_Fault_Cb,         NULL, &s_seq.data );

    qQueue_Setup( &s_seq.sigQueue, s_seq.sigStack,
                  sizeof( qSM_Signal_t ), qFLM_ArraySize( s_seq.sigStack ) );
    qStateMachine_InstallSignalQueue( &s_seq.machine, &s_seq.sigQueue );
    qStateMachine_InstallTimeoutSpec( &s_seq.machine, &s_seq.timeoutSpec );

    /* QuarkTS declares a non-const pointer here but treats the table as     */
    /* read-only; cast away const so the tables stay in flash.               */
    qStateMachine_Set_StateTimeouts( &stPwrUp,      (qSM_TimeoutStateDefinition_t *)s_toPwrUp,     qFLM_ArraySize( s_toPwrUp ) );
    qStateMachine_Set_StateTimeouts( &stBootSetup,  (qSM_TimeoutStateDefinition_t *)s_toSetup,     qFLM_ArraySize( s_toSetup ) );
    qStateMachine_Set_StateTimeouts( &stBootWaitD,  (qSM_TimeoutStateDefinition_t *)s_toEnum,      qFLM_ArraySize( s_toEnum ) );
    qStateMachine_Set_StateTimeouts( &stPulseReset, (qSM_TimeoutStateDefinition_t *)s_toPulse,     qFLM_ArraySize( s_toPulse ) );
    qStateMachine_Set_StateTimeouts( &stShutdown,   (qSM_TimeoutStateDefinition_t *)s_toShutdown,  qFLM_ArraySize( s_toShutdown ) );

    /* FSM task polls every 10 ms; all sequencing delays are >= 5 ms         */
    (void)qOS_Add_StateMachineTask( &s_seq.task, &s_seq.machine, qMedium_Priority,
                                    (qTime_t)0.01f, qEnabled, NULL );

    /* Cold autoboot: no host interaction required to bring TRES up          */
    TRES_Seq_RequestPowerOn( TRES_MODE_NORMAL );
}
