/**
  ******************************************************************************
  * @file    fsm_demo.c
  * @brief   Multiple concurrent QuarkTS state machines (RAM-usage comparison)
  *
  * A single hardware model is instantiated N times (see FSM_NUM_INSTANCES) as
  * fully independent state machines running under the same cooperative
  * scheduler. All instances share one set of state-callback functions and one
  * supervisor task; each instance owns its own qSM_t, state objects, signal
  * queue, timeout-specification and task node. Therefore the *marginal* RAM
  * cost of adding one more concurrent state machine is easy to measure:
  *
  *     RAM(instance) = qTask_t + qSM_t
  *                   + 5 * qSM_State_t
  *                   + FSM_InstanceData_t
  *                   + qQueue_t + 8 * qSM_Signal_t
  *                   + qSM_TimeoutSpec_t
  *
  * (the transition/timeout logic lives in shared functions / flash tables, so
  * it does not scale with the number of instances).
  *
  * State model per instance (drives PF3 on instance 0):
  *
  *     IDLE --EVT_START--> RUNNING(RUN_FAST) --fastend(timeout)--> RUN_SLOW
  *       ^                                      |                       |
  *       |<----------------EVT_STOP------------+                       |
  *       |<-----------EVT_RESET (guarded)-----ERROR<----EVT_FAULT------+
  *
  * A single 1 Hz supervisor task pushes the same scripted EVT_* signals into
  * every instance's signal-queue to prove all machines run in lock-step.
  ******************************************************************************
  */
#include "main.h"
#include "QuarkTS.h"
#include "fsm_demo.h"
#include "qport_py32.h"

/*============================================================================*/
/* Tune this to compare RAM usage (override with -D at build time too)        */
/*============================================================================*/
#ifndef FSM_NUM_INSTANCES
    #define FSM_NUM_INSTANCES   4U
#endif

/* User-defined signals                                                        */
#define EVT_START   ((qSM_SigId_t)1u)   /**< Boot request                     */
#define EVT_STOP    ((qSM_SigId_t)2u)   /**< Normal stop request               */
#define EVT_FAULT   ((qSM_SigId_t)3u)   /**< Fault / failure notification      */
#define EVT_RESET   ((qSM_SigId_t)4u)   /**< Recover from error (guarded)      */

/* Built-in timeout signals                                                    */
#define SIG_BLINK   (QSM_SIGNAL_TIMEOUT(0))  /**< Periodic LED toggle tick     */
#define SIG_FASTEND (QSM_SIGNAL_TIMEOUT(1))  /**< One-shot "fast phase done"   */

/*============================================================================*/
/* Per-instance runtime data (accessed from shared callbacks via h->StateData) */
/*============================================================================*/
typedef struct {
    qUINT32_t errorEntryTick;   /* tick captured when the error state was entered */
    qBool_t   inError;          /* true while inside the ERROR state               */
    qBool_t   isLedOwner;       /* only instance 0 physically drives PF3           */
    qUINT32_t resets;           /* count of completed RESET recoveries              */
} FSM_InstanceData_t;

/*============================================================================*/
/* Full object set owned by one state-machine instance                        */
/*============================================================================*/
typedef struct {
    qTask_t           task;        /* state-machine task node (registered once)  */
    qSM_t             machine;     /* the FSM engine                            */
    qSM_State_t       stateIdle;
    qSM_State_t       stateRunning;   /* composite (parent) state                */
    qSM_State_t       stateRunFast;
    qSM_State_t       stateRunSlow;
    qSM_State_t       stateError;
    FSM_InstanceData_t data;
    qQueue_t          sigQueue;    /* async signal queue                         */
    qSM_Signal_t      sigStack[ 8 ];
    qSM_TimeoutSpec_t timeoutSpec;
} FSM_Instance_t;

static FSM_Instance_t s_fsm[ FSM_NUM_INSTANCES ];

/*============================================================================*/
/* LED primitive (only the owning instance physically writes PF3)             */
/*============================================================================*/
static void Led_On( void )
{
    LL_GPIO_SetOutputPin( GPIOF, LL_GPIO_PIN_3 );
}

static void Led_Off( void )
{
    LL_GPIO_ResetOutputPin( GPIOF, LL_GPIO_PIN_3 );
}

static void Led_Toggle( void )
{
    LL_GPIO_TogglePin( GPIOF, LL_GPIO_PIN_3 );
}

/*============================================================================*/
/* Shared state callbacks -- one implementation for every instance            */
/* Each receives the owning instance via h->StateData.                        */
/*============================================================================*/
static FSM_Instance_t *FSM_Instance_Of( qSM_Handler_t h )
{
    /* every state is subscribed with pData = &instance, so StateData is it.  */
    return (FSM_Instance_t *)h->StateData;
}

static qSM_Status_t fsm_Idle_Cb( qSM_Handler_t h )
{
    FSM_Instance_t *inst = FSM_Instance_Of( h );

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            if ( inst->data.isLedOwner ) { Led_Off(); }
            break;
        case EVT_START:
            h->NextState = &inst->stateRunning;
            return qSM_STATUS_SIGNAL_HANDLED;
        case EVT_FAULT:
            h->NextState = &inst->stateError;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t fsm_Running_Cb( qSM_Handler_t h )
{
    (void)h;                            /* composite parent: structural only */
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t fsm_RunFast_Cb( qSM_Handler_t h )
{
    FSM_Instance_t *inst = FSM_Instance_Of( h );

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            /* entering fast phase ... (nothing specific here) */
            break;
        case SIG_BLINK:                 /* fast blink 0.25 s                 */
            if ( inst->data.isLedOwner ) { Led_Toggle(); }
            break;
        case SIG_FASTEND:               /* time is up -> slow phase          */
            h->NextState = &inst->stateRunSlow;
            return qSM_STATUS_SIGNAL_HANDLED;
        case EVT_STOP:
            h->NextState = &inst->stateIdle;
            return qSM_STATUS_SIGNAL_HANDLED;
        case EVT_FAULT:
            h->NextState = &inst->stateError;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t fsm_RunSlow_Cb( qSM_Handler_t h )
{
    FSM_Instance_t *inst = FSM_Instance_Of( h );

    switch ( h->Signal ) {
        case SIG_BLINK:                 /* slow blink 1.0 s                  */
            if ( inst->data.isLedOwner ) { Led_Toggle(); }
            break;
        case EVT_STOP:
            h->NextState = &inst->stateIdle;
            return qSM_STATUS_SIGNAL_HANDLED;
        case EVT_FAULT:
            h->NextState = &inst->stateError;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

static qSM_Status_t fsm_Error_Cb( qSM_Handler_t h )
{
    FSM_Instance_t *inst = FSM_Instance_Of( h );

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            if ( inst->data.isLedOwner ) { Led_On(); }
            inst->data.errorEntryTick = QOS_Tick_Get();
            inst->data.inError = qTrue;
            break;
        case QSM_SIGNAL_EXIT:
            inst->data.inError = qFalse;
            inst->data.resets++;
            break;
        case EVT_RESET:
            h->NextState = &inst->stateIdle;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/*============================================================================*/
/* Guard: allow EVT_RESET only after the error state held >= 3 s. Shared and  */
/* instance-safe because it reads h->StateData (already prepared by the       */
/* engine for the current state).                                             */
/*============================================================================*/
static qBool_t fsm_Reset_Guard( qSM_Handler_t h )
{
    FSM_InstanceData_t *d = &FSM_Instance_Of( h )->data;

    if ( ( qTrue == d->inError ) &&
         ( ( QOS_Tick_Get() - d->errorEntryTick ) >= 3000UL ) ) {
        return qTrue;                   /* allow the transition */
    }
    return qFalse;                      /* reject it for now    */
}

/*============================================================================*/
/* Per-state timeout specifications (const -> flash, shared by every instance)*/
/*============================================================================*/
static const qSM_TimeoutStateDefinition_t s_fastTO[] = {
    { 0.25f, QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT | QSM_TSOPT_PERIODIC }, /* SIG_BLINK   */
    { 4.0f,  QSM_TSOPT_INDEX(1) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },                        /* SIG_FASTEND */
};
static const qSM_TimeoutStateDefinition_t s_slowTO[] = {
    { 1.0f,  QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT | QSM_TSOPT_PERIODIC },   /* SIG_BLINK   */
};

/*============================================================================*/
/* Supervisor: one 1 Hz task pushes the same scripted signals into every      */
/* instance's signal-queue, proving all machines run in parallel (lock-step).  */
/*============================================================================*/
typedef struct {
    qUINT32_t   at;     /* elapsed seconds when the signal must be fired */
    qSM_SigId_t sig;
} ScriptStep_t;

static const ScriptStep_t s_script[] = {
    {  1,  EVT_START },
    {  8,  EVT_STOP  },
    {  9,  EVT_START },
    { 15,  EVT_FAULT },
    { 20,  EVT_RESET },
};
#define SCRIPT_LEN ( (qUINT32_t)( sizeof( s_script ) / sizeof( s_script[ 0 ] ) ) )

static void Supervisor_Cb( qEvent_t e )
{
    static qUINT32_t elapsedSec = 0U;
    static qUINT32_t step = 0U;
    qUINT32_t i;

    (void)e;
    elapsedSec++;

    if ( ( step < SCRIPT_LEN ) && ( elapsedSec == s_script[ step ].at ) ) {
        for ( i = 0U; i < FSM_NUM_INSTANCES; i++ ) {
            (void)qStateMachine_SendSignal( &s_fsm[ i ].machine,
                                            s_script[ step ].sig, NULL, qFalse );
        }
        step++;
    }

    if ( elapsedSec > s_script[ SCRIPT_LEN - 1U ].at ) {   /* loop the script */
        elapsedSec = 0U;
        step = 0U;
    }
}

/*============================================================================*/
/* Public: create every instance and register its state-machine task, plus    */
/* the single supervisor task.                                                */
/*============================================================================*/
void FSM_Example_Init(void)
{
    static qTask_t s_supervisorTask;
    qUINT32_t i;

    for ( i = 0U; i < FSM_NUM_INSTANCES; i++ ) {
        FSM_Instance_t *inst = &s_fsm[ i ];

        /* instance 0 drives the physical PF3 LED; the rest are silent clones */
        inst->data.isLedOwner = ( i == 0U ) ? qTrue : qFalse;
        inst->data.inError    = qFalse;
        inst->data.resets     = 0U;

        /* Build the topology */
        qStateMachine_Setup( &inst->machine, NULL, &inst->stateIdle, NULL, NULL );

        qStateMachine_StateSubscribe( &inst->machine, &inst->stateIdle, NULL,
                                      fsm_Idle_Cb, NULL, inst );
        qStateMachine_StateSubscribe( &inst->machine, &inst->stateRunning, NULL,
                                      fsm_Running_Cb, &inst->stateRunFast, inst );
        qStateMachine_StateSubscribe( &inst->machine, &inst->stateRunFast, &inst->stateRunning,
                                      fsm_RunFast_Cb, NULL, inst );
        qStateMachine_StateSubscribe( &inst->machine, &inst->stateRunSlow, &inst->stateRunning,
                                      fsm_RunSlow_Cb, NULL, inst );
        qStateMachine_StateSubscribe( &inst->machine, &inst->stateError, NULL,
                                      fsm_Error_Cb, NULL, inst );

        /* Async signal delivery + timeouts */
        qQueue_Setup( &inst->sigQueue, inst->sigStack, sizeof( qSM_Signal_t ),
                      qFLM_ArraySize( inst->sigStack ) );
        qStateMachine_InstallSignalQueue( &inst->machine, &inst->sigQueue );
        qStateMachine_InstallTimeoutSpec( &inst->machine, &inst->timeoutSpec );

        /* QuarkTS declares a non-const pointer here but treats the table as   */
        /* read-only; cast away const so the tables stay in flash.             */
        qStateMachine_Set_StateTimeouts( &inst->stateRunFast, (qSM_TimeoutStateDefinition_t *)s_fastTO,
                                         qFLM_ArraySize( s_fastTO ) );
        qStateMachine_Set_StateTimeouts( &inst->stateRunSlow, (qSM_TimeoutStateDefinition_t *)s_slowTO,
                                         qFLM_ArraySize( s_slowTO ) );

        /* Poll this machine every 100 ms */
        qOS_Add_StateMachineTask( &inst->task, &inst->machine, qMedium_Priority,
                                  (qTime_t)0.1f, qEnabled, NULL );
    }

    /* One shared supervisor feeds every machine */
    qOS_Add_Task( &s_supervisorTask, Supervisor_Cb, qHigh_Priority,
                  (qTime_t)1.0f, qPeriodic, qEnabled, NULL );
}