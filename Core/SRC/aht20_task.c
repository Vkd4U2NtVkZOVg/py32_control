/**
 * @file aht20_task.c
 * @brief AHT20 periodic dew-point measurement task (QuarkTS state machine).
 *
 * State machine (polled every 10 ms), aligned to the AHT20 datasheet flow:
 *
 *     [*] --> POWER_ON_WAIT --t_pwup--> CHECK_STATUS
 *     CHECK_STATUS --已校准(0x18)--> PRE_TRIGGER
 *     CHECK_STATUS --未校准/读失败--> SPECIAL_INIT --t_init--> CHECK_STATUS
 *     SPECIAL_INIT --失败--> SAMPLE_WAIT --t_cycle--> CHECK_STATUS
 *     PRE_TRIGGER --t_pre--> TRIGGER
 *     TRIGGER --> WAIT_MEASURE --t_conv--> READ_DATA
 *     READ_DATA --成功/失败--> SAMPLE_WAIT --t_cycle--> TRIGGER (loop)
 *
 *   POWER_ON_WAIT  waits the datasheet power-up settle (>= 100 ms) before the
 *                  first sensor access.
 *   CHECK_STATUS   reads the status register (0x71) and routes: calibrated
 *                  (status & 0x18 == 0x18) -> PRE_TRIGGER, otherwise -> SPECIAL_INIT.
 *   SPECIAL_INIT   one-time init: AHT20_Init() issues 0xBE when not calibrated;
 *                  the settle delay is the t_init timeout.
 *   PRE_TRIGGER    small datasheet settle (10 ms) before the measure trigger.
 *   TRIGGER        sends the 0xAC trigger on entry.
 *   WAIT_MEASURE   waits the 80 ms conversion time (t_conv), then READ_DATA.
 *   READ_DATA      reads 7 bytes + CRC + converts temperature/humidity on entry,
 *                  then computes and caches the dew point (only when the read
 *                  succeeded; cached data expires after 25 s).
 *   SAMPLE_WAIT    holds t_cycle, then TRIGGER on success or CHECK_STATUS on error.
 *
 * Every long delay is a state-machine timeout, so the task yields the CPU
 * between states; the only busy-work is the ~0.1 ms bit-banged I2C
 * transaction inside the driver functions.
 *
 * NOTE: never assign h->NextState inside a QSM_SIGNAL_ENTRY handler. The
 * QuarkTS engine calls the entry action via qStateMachine_StateOnEntry(),
 * which does not check NextState (only qStateMachine_StateOnSignal() does).
 * The decision states above therefore read/act in their ENTRY handler and set
 * h->NextState in the periodic-poll (QSM_SIGNAL_NONE) branch, which the engine
 * does deliver through StateOnSignal. All fixed delays use timeout tables.
 */
#include "main.h"
#include "qport_py32.h"
#include "QuarkTS.h"
#include "aht20_task.h"
#include "aht20.h"

#include <limits.h>   /* INT32_MIN */

/*============================================================================*/
/* Sequencing timings (seconds, QuarkTS time base = 1 kHz)                    */
/*============================================================================*/
#define AHT20_T_PWUP_S      (0.100f)   /* power-up settle >= 100 ms (SSD)  */
#define AHT20_T_INIT_S      (0.030f)   /* settle after calibration cmd      */
#define AHT20_T_PRE_S       (0.010f)   /* pre-trigger settle (datasheet)    */
#define AHT20_T_CYCLE_S     (10.0f)    /* measurement period                 */
#define AHT20_T_CONV_S      (0.080f)   /* AHT20 conversion wait (datasheet) */

#define AHT20_TIMEOUT       (QSM_SIGNAL_TIMEOUT(0))  /**< one-shot timeout   */

/*============================================================================*/
/* Per-task runtime data (state callbacks access the task via this singleton) */
/*============================================================================*/
typedef struct {
    qTask_t           task;
    qSM_t             machine;
    qSM_TimeoutSpec_t timeoutSpec;
    qQueue_t          sigQueue;
    qSM_Signal_t      sigStack[8];
    int32_t           latestDewPointC;  /**< cached, read by the getter      */
    int32_t           latestTempC;      /**< cached ambient temp (milli-°C)  */
    uint32_t          sampleTick;
    volatile uint8_t  sampleValid;
    int               lastStatus;       /**< last AHT20_* result code        */
    AHT20_Data_t      lastData;         /**< last successful measurement     */
    uint8_t           statusByte;       /**< raw status from CHECK_STATUS    */
} AHT20_Task_t;

static AHT20_Task_t s_aht;

static qSM_State_t stPowerOnWait, stCheckStatus, stSpecialInit,
                   stPreTrigger, stTrigger, stWaitMeasure,
                   stReadData, stSampleWait;

/*============================================================================*/
/* State callbacks                                                            */
/*============================================================================*/
/* POWER_ON_WAIT: no sensor access until the t_pwup timeout, so the datasheet
 * >=100 ms power-up settle is fully yielded to the scheduler. */
static qSM_Status_t st_PowerOnWait_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case AHT20_TIMEOUT:
            h->NextState = &stCheckStatus;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* CHECK_STATUS: read status once on entry, route on the next poll. */
static qSM_Status_t st_CheckStatus_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_aht.lastStatus = AHT20_ReadStatus( &s_aht.statusByte );
            return qSM_STATUS_EXIT_SUCCESS;
        default:
            break;
    }
    /* QSM_SIGNAL_NONE poll: decide the branch (entry read already done). */
    if ( ( s_aht.lastStatus == AHT20_OK ) &&
         ( ( s_aht.statusByte & AHT20_STATUS_CALIBRATED ) == AHT20_STATUS_CALIBRATED ) ) {
        h->NextState = &stPreTrigger;
    } else {
        h->NextState = &stSpecialInit;
    }
    return qSM_STATUS_SIGNAL_HANDLED;
}

/* SPECIAL_INIT: AHT20_Init reads status internally and issues 0xBE when the
 * calibration bits (0x18) are not set; t_init settles it. */
static qSM_Status_t st_SpecialInit_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_aht.lastStatus = AHT20_Init();
            break;
        case AHT20_TIMEOUT:
            h->NextState = (s_aht.lastStatus == AHT20_OK) ? &stCheckStatus : &stSampleWait;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* PRE_TRIGGER: small datasheet settle before the measure trigger. */
static qSM_Status_t st_PreTrigger_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case AHT20_TIMEOUT:
            h->NextState = &stTrigger;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* TRIGGER: send 0xAC on entry, then head to the conversion wait. */
static qSM_Status_t st_Trigger_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_aht.lastStatus = AHT20_StartMeasure();
            return qSM_STATUS_EXIT_SUCCESS;
        default:
            break;
    }
    h->NextState = (s_aht.lastStatus == AHT20_OK) ? &stWaitMeasure : &stSampleWait;
    return qSM_STATUS_SIGNAL_HANDLED;
}

/* WAIT_MEASURE: fixed 80 ms conversion wait (no busy polling). */
static qSM_Status_t st_WaitMeasure_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case AHT20_TIMEOUT:
            h->NextState = &stReadData;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* READ_DATA: read + decode on entry; on success compute and cache the dew
 * point. Success or failure -> SAMPLE_WAIT (period cycle). */
static qSM_Status_t st_ReadData_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_aht.lastStatus = AHT20_ReadResult( &s_aht.lastData );
            if ( s_aht.lastStatus == AHT20_OK ) {
                s_aht.sampleValid = 0u;
                s_aht.sampleTick = QOS_Tick_Get();
                s_aht.latestTempC      = s_aht.lastData.temperature_milli_celsius;
                s_aht.latestDewPointC = AHT20_DewPointMilliCelsius(
                    s_aht.lastData.temperature_milli_celsius,
                    s_aht.lastData.humidity_milli_percent );
                s_aht.sampleValid = 1u;
            }
            return qSM_STATUS_EXIT_SUCCESS;
        default:
            break;
    }
    h->NextState = &stSampleWait;
    return qSM_STATUS_SIGNAL_HANDLED;
}

/* SAMPLE_WAIT: hold the measurement period, then trigger the next read. */
static qSM_Status_t st_SampleWait_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case AHT20_TIMEOUT:
            h->NextState = (s_aht.lastStatus == AHT20_OK) ? &stTrigger : &stCheckStatus;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/*============================================================================*/
/* Const timeout specifications (flash-resident, one one-shot timer per state)*/
/*============================================================================*/
static const qSM_TimeoutStateDefinition_t s_toPwup[] = {
    { (qTime_t)AHT20_T_PWUP_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toInit[] = {
    { (qTime_t)AHT20_T_INIT_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toPre[] = {
    { (qTime_t)AHT20_T_PRE_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toConv[] = {
    { (qTime_t)AHT20_T_CONV_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toCycle[] = {
    { (qTime_t)AHT20_T_CYCLE_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};

/*============================================================================*/
/* Public interface                                                           */
/*============================================================================*/
void AHT20_Task_Init( void )
{
    s_aht.sampleValid = 0u;
    s_aht.latestDewPointC = INT32_MIN;
    s_aht.latestTempC     = INT32_MIN;
    s_aht.lastStatus      = AHT20_ERR_NOT_READY;
    s_aht.statusByte      = 0u;

    qStateMachine_Setup( &s_aht.machine, NULL, &stPowerOnWait, NULL, NULL );

    qStateMachine_StateSubscribe( &s_aht.machine, &stPowerOnWait, NULL, st_PowerOnWait_Cb, NULL, NULL );
    qStateMachine_StateSubscribe( &s_aht.machine, &stCheckStatus, NULL, st_CheckStatus_Cb, NULL, NULL );
    qStateMachine_StateSubscribe( &s_aht.machine, &stSpecialInit, NULL, st_SpecialInit_Cb, NULL, NULL );
    qStateMachine_StateSubscribe( &s_aht.machine, &stPreTrigger,  NULL, st_PreTrigger_Cb,  NULL, NULL );
    qStateMachine_StateSubscribe( &s_aht.machine, &stTrigger,     NULL, st_Trigger_Cb,     NULL, NULL );
    qStateMachine_StateSubscribe( &s_aht.machine, &stWaitMeasure, NULL, st_WaitMeasure_Cb, NULL, NULL );
    qStateMachine_StateSubscribe( &s_aht.machine, &stReadData,    NULL, st_ReadData_Cb,    NULL, NULL );
    qStateMachine_StateSubscribe( &s_aht.machine, &stSampleWait,  NULL, st_SampleWait_Cb,  NULL, NULL );

    qQueue_Setup( &s_aht.sigQueue, s_aht.sigStack,
                  sizeof( qSM_Signal_t ), qFLM_ArraySize( s_aht.sigStack ) );
    qStateMachine_InstallSignalQueue( &s_aht.machine, &s_aht.sigQueue );
    qStateMachine_InstallTimeoutSpec( &s_aht.machine, &s_aht.timeoutSpec );

    /* Only the states that hold a fixed delay get a timeout table. */
    qStateMachine_Set_StateTimeouts( &stPowerOnWait, (qSM_TimeoutStateDefinition_t *)s_toPwup,  qFLM_ArraySize( s_toPwup ) );
    qStateMachine_Set_StateTimeouts( &stSpecialInit, (qSM_TimeoutStateDefinition_t *)s_toInit,   qFLM_ArraySize( s_toInit ) );
    qStateMachine_Set_StateTimeouts( &stPreTrigger,  (qSM_TimeoutStateDefinition_t *)s_toPre,    qFLM_ArraySize( s_toPre ) );
    qStateMachine_Set_StateTimeouts( &stWaitMeasure, (qSM_TimeoutStateDefinition_t *)s_toConv,   qFLM_ArraySize( s_toConv ) );
    qStateMachine_Set_StateTimeouts( &stSampleWait,  (qSM_TimeoutStateDefinition_t *)s_toCycle,  qFLM_ArraySize( s_toCycle ) );

    /* 10 ms poll; lowest priority so it never disturbs the TRES sequencer. */
    (void)qOS_Add_StateMachineTask( &s_aht.task, &s_aht.machine, qLowest_Priority,
                                    (qTime_t)0.01f, qEnabled, NULL );
}

/* Do not modify lastStatus here: the I2C ISR also calls the getters, and
 * lastStatus belongs to the FSM's in-flight init/trigger result. */
static uint8_t aht_sample_fresh(void)
{
    uint32_t primask = __get_PRIMASK();
    uint8_t valid;
    __disable_irq();
    if ((uint32_t)(QOS_Tick_Get() - s_aht.sampleTick) >= 25000u) {
        s_aht.sampleValid = 0u;
    }
    valid = s_aht.sampleValid;
    __set_PRIMASK(primask);
    return valid;
}

int32_t AHT20_Get_LastDewPointMilliCelsius(void)
{
    return aht_sample_fresh() ? s_aht.latestDewPointC : INT32_MIN;
}

int32_t AHT20_Get_LastTempMilliCelsius(void)
{
    return aht_sample_fresh() ? s_aht.latestTempC : INT32_MIN;
}

int AHT20_Get_LastStatus(void)
{
    if (s_aht.lastStatus != AHT20_OK) return s_aht.lastStatus;
    return aht_sample_fresh() ? AHT20_OK : AHT20_ERR_NOT_READY;
}
