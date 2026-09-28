/**
  ******************************************************************************
  * @file    tres_seq.c
  * @brief   TRES (STM32H7) boot/reset/DFU sequencer state machine (QuarkTS).
  *
  * State machine (polled every 10 ms), no power management:
  *
  *     [*] --> PWR_UP --t_rail--> BOOT_WAIT_NORMAL --SOM_BOOTED--> RUNNING
  *     RUNNING --CMD_RESET--> PULSE_RESET_NORMAL --> BOOT_WAIT_NORMAL
  *     RUNNING --CMD_ENTER_DFU--> PULSE_RESET_DFU --> BOOT_WAIT_DFU
  *                       --t_enum--> DFU_ACTIVE --CMD_RESET--> PULSE_RESET_NORMAL
  *
  *   PWR_UP          asserts PWR_EN=1, BOOT0=0, VDD33USB=0; RST stays asserted
  *                   (from GPIO init) until BOOT_WAIT_NORMAL releases it.
  *   BOOT_WAIT_NORMAL releases RST and waits for the whole boot chain: a fresh
  *                   SOM_BOOTED rising edge, or (host already high) the t_appboot
  *                   grace, confirms the chain -> RUNNING. No supervision timeout.
  *   RUNNING         idle; samples SOM_BOOTED to keep host_ready, and accepts
  *                   CMD_RESET (-> NORMAL) / CMD_ENTER_DFU (-> DFU).
  *   PULSE_RESET_NORMAL asserts RST with BOOT0=0, VDD33USB=0; fixed target
  *                   BOOT_WAIT_NORMAL after t_rst.
  *   PULSE_RESET_DFU asserts RST with BOOT0=1, VDD33USB=1; fixed target
  *                   BOOT_WAIT_DFU after t_rst.
  *   BOOT_WAIT_DFU   releases RST (BOOT0=1 latched); t_enum enumeration window
  *                   -> DFU_ACTIVE.
  *   DFU_ACTIVE      waits for the host dfu-util session; VDD33USB stays 1.
  *                   CMD_RESET -> PULSE_RESET_NORMAL (entry clears BOOT0/USB).
  *
  * The reset pulse is split into two states whose target is fixed: before their
  * t_rst timeout they assert RST and set BOOT0/VDD33USB, then both states leave
  * for a single, predetermined next state (NORMAL -> BOOT_WAIT_NORMAL,
  * DFU -> BOOT_WAIT_DFU). No level-based routing is needed, so BOOT0 is set in
  * the state entry rather than by the source state.
  *
  * Every long delay is a state-machine timeout, so the task yields the CPU
  * between states; fixed one-shot delays (t_rail / t_rst / t_enum) use const
  * timeout-spec tables in flash, like the s_fastTO[] pattern of fsm_demo.c.
  * The machine is polled every 10 ms; QuarkTS invokes the current state
  * callback on every task cycle (Signal == QSM_SIGNAL_NONE when nothing is
  * queued), so tick-based supervision (t_appboot grace) works without extra
  * wake-up signals.
  *
  * NOTE: never assign h->NextState inside a QSM_SIGNAL_ENTRY handler. The
  * QuarkTS engine calls the entry action via qStateMachine_StateOnEntry(),
  * which does not check NextState (only qStateMachine_StateOnSignal() does).
  * The sampling states above (BOOT_WAIT_NORMAL, RUNNING) therefore set
  * h->NextState in the periodic-poll (QSM_SIGNAL_NONE) branch, which the engine
  * does deliver through StateOnSignal. All fixed delays use timeout tables.
  ******************************************************************************
  */
#include "main.h"
#include "QuarkTS.h"
#include "tres_seq.h"
#include "qport_py32.h"

/*============================================================================*/
/* Internal signals                                                           */
/*============================================================================*/
#define SIG_RESET        ((qSM_SigId_t)1u)
#define SIG_ENTER_DFU    ((qSM_SigId_t)2u)
#define SIG_SEQ_TIMEOUT  (QSM_SIGNAL_TIMEOUT(0))  /**< one-shot sequencing */

/*============================================================================*/
/* Per-machine runtime data (h->StateData of every state callback)            */
/*============================================================================*/
typedef struct {
    uint8_t   hostReady;    /**< live SOM_BOOTED level                         */
    uint8_t   somPrev;      /**< previous SOM_BOOTED level (edge detection)    */
    uint8_t   somRise;      /**< consumed by BOOT_WAIT_NORMAL                  */
    uint8_t   somFall;      /**< unused here, kept for symmetry               */
    uint8_t   stateId;      /**< TRES_State_t mirror, updated on each entry    */
    qUINT32_t entryTick;    /**< tick captured at state entry                  */
} TRES_Data_t;

static qSM_State_t stPwrUp, stBootWaitN, stRunning,
                   stPulseResetNormal, stPulseResetDfu,
                   stBootWaitD, stDfuActive;

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
static void Pins_PwrOn( void )
{
    LL_GPIO_SetOutputPin( GPIOB, TRES_PWR_EN_Pin );
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
        d->somRise   = ( now != 0u ) ? 1u : 0u;
        d->somFall   = ( now == 0u ) ? 1u : 0u;
        d->somPrev   = now;
        d->hostReady = now;
    }
}

/*============================================================================*/
/* State callbacks (flat hierarchy, all states run on every 10 ms cycle)      */
/*============================================================================*/
static qSM_Status_t st_PwrUp_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_PWR_UP;
            Pins_PwrOn();              /* PWR_EN=1                           */
            Pins_Boot0( 0u );          /* cold boot is always NORMAL          */
            Pins_VddUsb( 0u );         /* USB rail off until a DFU request    */
            /* RST stays asserted (0) from GPIO init until BOOT_WAIT release  */
            break;
        case SIG_SEQ_TIMEOUT:
            h->NextState = &stBootWaitN;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_BootWaitNormal_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId   = (uint8_t)TRES_BOOT_WAIT_NORMAL;
            d->entryTick = QOS_Tick_Get();
            Pins_RstRelease();             /* BOOT0=0 is latched by the H7 here   */
            /* capture the current level so a pre-asserted host (reset path)     */
            /* does not look like a fresh edge                                    */
            d->somPrev   = Pins_SomBooted();
            d->hostReady = d->somPrev;
            d->somRise   = 0u;
            d->somFall   = 0u;
            return qSM_STATUS_EXIT_SUCCESS;
        default:
            break;
    }

    /* periodic supervision: wait for the whole boot chain, no timeout (C1). */
    Som_Sample( d );
    if ( d->somRise != 0u ) {
        /* TRES booted and started the host: chain confirmed                 */
        h->NextState = &stRunning;
        return qSM_STATUS_SIGNAL_HANDLED;
    }
    if ( d->somPrev != 0u ) {
        /* reset/reboot with a live host: no fresh edge, use grace only      */
        if ( ( QOS_Tick_Get() - d->entryTick ) >= TRES_T_APPBOOT_MS ) {
            h->NextState = &stRunning;
            return qSM_STATUS_SIGNAL_HANDLED;
        }
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_Running_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_RUNNING;
            d->hostReady = Pins_SomBooted();
            d->somPrev   = d->hostReady;
            d->somRise   = 0u;
            d->somFall   = 0u;
            return qSM_STATUS_EXIT_SUCCESS;
        case SIG_RESET:                /* normal re-boot: steps to NORMAL    */
            h->NextState = &stPulseResetNormal;
            return qSM_STATUS_SIGNAL_HANDLED;
        case SIG_ENTER_DFU:            /* DFU entry: steps to DFU pulse      */
            h->NextState = &stPulseResetDfu;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }

    /* keep host_ready fresh in STATUS                                       */
    Som_Sample( d );
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_PulseResetNormal_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_PULSE_RESET_NORMAL;
            Pins_Boot0( 0u );          /* boot NORMAL, USB rail off          */
            Pins_VddUsb( 0u );
            Pins_RstAssert();          /* pulse low, released after t_rst    */
            break;
        case SIG_SEQ_TIMEOUT:
            h->NextState = &stBootWaitN;   /* fixed target                    */
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_PulseResetDfu_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_PULSE_RESET_DFU;
            Pins_Boot0( 1u );          /* boot to system DFU, USB rail on    */
            Pins_VddUsb( 1u );
            Pins_RstAssert();          /* pulse low, released after t_rst    */
            break;
        case SIG_SEQ_TIMEOUT:
            h->NextState = &stBootWaitD;   /* fixed target                    */
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_BootWaitDfu_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_BOOT_WAIT_DFU;
            Pins_RstRelease();         /* BOOT0=1 latched: system DFU starts */
            break;
        case SIG_SEQ_TIMEOUT:
            h->NextState = &stDfuActive;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t st_DfuActive_Cb( qSM_Handler_t h )
{
    TRES_Data_t *d = (TRES_Data_t *)h->StateData;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            d->stateId = (uint8_t)TRES_DFU_ACTIVE;
            /* VDD33USB must stay 1; host drives the DFU session             */
            break;
        case SIG_RESET:                /* flash done: exit DFU, boot app     */
            h->NextState = &stPulseResetNormal;  /* entry clears BOOT0/USB   */
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/*============================================================================*/
/* Const timeout specifications (flash-resident, one one-shot timer per state)*/
/*============================================================================*/
static const qSM_TimeoutStateDefinition_t s_toRail[] = {
    { (qTime_t)( (float)TRES_T_RAIL_MS / 1000.0f ),
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toPulse[] = {
    { (qTime_t)( (float)TRES_T_RST_MS / 1000.0f ),
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toEnum[] = {
    { (qTime_t)( (float)TRES_T_ENUM_MS / 1000.0f ),
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};

/*============================================================================*/
/* Public request interface: command layer -> FSM signal queue                */
/*============================================================================*/
static void Seq_Send( qSM_SigId_t sig )
{
    (void)qStateMachine_SendSignal( &s_seq.machine, sig, NULL, qFalse );
}

void TRES_Seq_RequestReset( void )
{
    Seq_Send( SIG_RESET );
}

void TRES_Seq_RequestEnterDfu( void )
{
    Seq_Send( SIG_ENTER_DFU );
}

/*============================================================================*/
/* Status interface                                                           */
/*============================================================================*/
uint8_t TRES_Seq_GetState( void )       { return s_seq.data.stateId; }
uint8_t TRES_Seq_HostReady( void )      { return s_seq.data.hostReady; }

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
/* Public: build the topology and register the task. Initial state is PWR_UP, */
/* so cold boot runs automatically with no injected signal.                   */
/*============================================================================*/
void TRES_Seq_Init( void )
{
    s_seq.data.hostReady = 0u;
    s_seq.data.somPrev   = 0u;
    s_seq.data.somRise   = 0u;
    s_seq.data.somFall   = 0u;
    s_seq.data.stateId   = (uint8_t)TRES_PWR_UP;
    s_seq.data.entryTick = 0u;

    qStateMachine_Setup( &s_seq.machine, NULL, &stPwrUp, NULL, NULL );

    qStateMachine_StateSubscribe( &s_seq.machine, &stPwrUp,      NULL, st_PwrUp_Cb,           NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stBootWaitN,  NULL, st_BootWaitNormal_Cb,  NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stRunning,           NULL, st_Running_Cb,         NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stPulseResetNormal,  NULL, st_PulseResetNormal_Cb, NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stPulseResetDfu,     NULL, st_PulseResetDfu_Cb,    NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stBootWaitD,  NULL, st_BootWaitDfu_Cb,     NULL, &s_seq.data );
    qStateMachine_StateSubscribe( &s_seq.machine, &stDfuActive,  NULL, st_DfuActive_Cb,       NULL, &s_seq.data );

    qQueue_Setup( &s_seq.sigQueue, s_seq.sigStack,
                  sizeof( qSM_Signal_t ), qFLM_ArraySize( s_seq.sigStack ) );
    qStateMachine_InstallSignalQueue( &s_seq.machine, &s_seq.sigQueue );
    qStateMachine_InstallTimeoutSpec( &s_seq.machine, &s_seq.timeoutSpec );

    /* QuarkTS declares a non-const pointer here but treats the table as     */
    /* read-only; cast away const so the tables stay in flash.               */
    qStateMachine_Set_StateTimeouts( &stPwrUp,      (qSM_TimeoutStateDefinition_t *)s_toRail,  qFLM_ArraySize( s_toRail ) );
    qStateMachine_Set_StateTimeouts( &stPulseResetNormal, (qSM_TimeoutStateDefinition_t *)s_toPulse, qFLM_ArraySize( s_toPulse ) );
    qStateMachine_Set_StateTimeouts( &stPulseResetDfu,    (qSM_TimeoutStateDefinition_t *)s_toPulse, qFLM_ArraySize( s_toPulse ) );
    qStateMachine_Set_StateTimeouts( &stBootWaitD,  (qSM_TimeoutStateDefinition_t *)s_toEnum,  qFLM_ArraySize( s_toEnum ) );

    /* FSM task polls every 10 ms; all sequencing delays are >= 5 ms         */
    (void)qOS_Add_StateMachineTask( &s_seq.task, &s_seq.machine, qMedium_Priority,
                                    (qTime_t)0.01f, qEnabled, NULL );
}
