/**
 * @file sdm845_boot.c
 * @brief SDM845 boot/reset/strap state machine (QuarkTS).
 *
 * State machine (polled every 10 ms), two-stage boot detection:
 *
 *     [*] --> POWER_ON_WAIT --t_pwup(100ms)--> PWR_PRESS
 *     PWR_PRESS --t_hold(3s)--> BOOT_CHECK
 *     BOOT_CHECK --stage1 "Loader Build Info"--> STAGE2_WAIT
 *     BOOT_CHECK --15s 无响应 & retry<MAX--> BOOT_RESET
 *     BOOT_CHECK --15s 无响应 & retry>=MAX--> STRAP_FAIL (terminal)
 *     STAGE2_WAIT --stage2 "login" + idle--> BOOT_SUCCESS (done, terminal)
 *     STAGE2_WAIT --t_login(60s) & retry<MAX--> BOOT_RESET
 *     STAGE2_WAIT --t_login(60s) & retry>=MAX--> STRAP_FAIL (terminal)
 *     BOOT_RESET --t_kill(15s)--> PMIC_SETTLE
 *     PMIC_SETTLE --t_settle(3s)--> PWR_PRESS (retry loop)
 *
 * GPIO: PWRKEY = MIX3_BOOTKEY (PB7), RST_KEY = MIX3_RST (PB8), low = pressed.
 * Stage 1 ("Loader Build Info") proves the bootloader is alive; stage 2
 * ("login") proves Linux user space came up and gates BOOT_SUCCESS.
 * A loader without a login points at a kernel/init hang -> targeted retry.
 *
 * Key constraints honored:
 *   - Never set h->NextState in a QSM_SIGNAL_ENTRY handler (engine ignores it).
 *   - Decision states (BOOT_CHECK, STAGE2_WAIT) read the detector in the
 *     periodic-poll (QSM_SIGNAL_NONE) branch and set NextState there; fixed
 *     delays use const timeout tables. Success is checked BEFORE the stage
 *     timeout, so a late-arriving banner is not spuriously treated as failure.
 */
#include "main.h"
#include "QuarkTS.h"
#include "gpio.h"
#include "sdm845_boot.h"
#include "sdm845_detect.h"

/*============================================================================*/
/* Internal signal / timeout                                                  */
/*============================================================================*/
#define SDM845_TIMEOUT  (QSM_SIGNAL_TIMEOUT(0))   /**< one-shot sequencing */

/*============================================================================*/
/* Timings (seconds, QuarkTS time base = 1 kHz) + retry cap                    */
/*============================================================================*/
#define SDM845_T_PWUP_S     (0.100f)   /* power-up: hold keys released   */
#define SDM845_T_HOLD_S     (3.0f)     /* hold PWRKEY low to power on    */
#define SDM845_T_BOOTCHK_S  (15.0f)    /* stage 1: wait for loader banner */
#define SDM845_T_LOGIN_S    (60.0f)    /* stage 2: wait for login prompt  */
#define SDM845_T_KILL_S     (15.0f)    /* hard-kill (PWR+RST low) pulse  */
#define SDM845_T_SETTLE_S   (3.0f)     /* PMIC power-cycle settle        */
#define SDM845_BOOT_MAX_RETRIES  (3u)

/*============================================================================*/
/* Runtime data (per-machine singleton)                                        */
/*============================================================================*/
typedef struct {
    qTask_t           task;
    qSM_t             machine;
    qSM_TimeoutSpec_t timeoutSpec;
    qQueue_t          sigQueue;
    qSM_Signal_t      sigStack[8];
    uint8_t           stateId;
    uint8_t           retryCount;
    uint8_t           booted;
} Sdm845_Boot_t;

static Sdm845_Boot_t s_boot;

static qSM_State_t stPowerOnWait, stPwrPress, stBootCheck, stStage2Wait,
                   stBootSuccess, stBootReset, stPmicSettle, stStrapFail;

/*============================================================================*/
/* Pin primitives (the ONLY code that touches the SDM845 control lines)        */
/*============================================================================*/
static void Pin_PwrKey( uint8_t pressed )
{
    if ( pressed != 0u ) { LL_GPIO_ResetOutputPin( GPIOB, MIX3_BOOTKEY_Pin ); }
    else                { LL_GPIO_SetOutputPin( GPIOB, MIX3_BOOTKEY_Pin ); }
}

static void Pin_RstKey( uint8_t pressed )
{
    if ( pressed != 0u ) { LL_GPIO_ResetOutputPin( GPIOB, MIX3_RST_Pin ); }
    else                { LL_GPIO_SetOutputPin( GPIOB, MIX3_RST_Pin ); }
}

/*============================================================================*/
/* State callbacks                                                             */
/*============================================================================*/
/* POWER_ON_WAIT: keys released, retries cleared. If the SoC is already
 * running (SOM_BOOTED high, e.g. this MCU rebooted alone), skip straight to
 * BOOT_SUCCESS instead of re-powering it. */
static qSM_Status_t st_PowerOnWait_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_POWER_ON_WAIT;
            s_boot.retryCount = 0u;
            Pin_PwrKey( 0u );             /* release (high) */
            Pin_RstKey( 0u );             /* release (high) */
            break;
        case SDM845_TIMEOUT:
            h->NextState = &stPwrPress;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            /* Periodic poll: SoC already alive takes priority over timeout. */
            if ( LL_GPIO_IsInputPinSet( GPIOB, SOM_BOOTED_Pin ) != 0u ) {
                h->NextState = &stBootSuccess;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* PWR_PRESS: assert PWRKEY low to power the SoC on. */
static qSM_Status_t st_PwrPress_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_PWR_PRESS;
            Pin_PwrKey( 1u );             /* pressed (low) */
            Pin_RstKey( 0u );             /* release */
            break;
        case SDM845_TIMEOUT:
            h->NextState = &stBootCheck;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* BOOT_CHECK: keys released on entry (fix: no exit-dependency), then on every
 * poll check the detector first (success priority); only the 15 s timeout
 * drives the failure branch (retry or strap-fail). */
static qSM_Status_t st_BootCheck_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_BOOT_CHECK;
            Pin_PwrKey( 0u );             /* release */
            Pin_RstKey( 0u );             /* release */
            break;
        case SDM845_TIMEOUT:
            if ( s_boot.retryCount < SDM845_BOOT_MAX_RETRIES ) {
                h->NextState = &stBootReset;
            } else {
                h->NextState = &stStrapFail;
            }
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            /* Periodic poll: stage 1 (loader banner) takes priority over the
             * 15 s timeout; stage 2 (login) is handled by STAGE2_WAIT. */
            if ( Sdm845_Detect_IsStage1Seen() != 0u ) {
                h->NextState = &stStage2Wait;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* STAGE2_WAIT: loader proved stage 1; now wait for the "login" prompt.
 * Boot completes only when stage 2 is seen and the line goes idle. */
static qSM_Status_t st_Stage2Wait_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_STAGE2_WAIT;
            break;
        case SDM845_TIMEOUT:
            /* Loader up but no login: kernel/init hang -> targeted retry. */
            if ( s_boot.retryCount < SDM845_BOOT_MAX_RETRIES ) {
                h->NextState = &stBootReset;
            } else {
                h->NextState = &stStrapFail;
            }
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            /* Periodic poll: full boot success takes priority over timeout. */
            if ( Sdm845_Detect_IsBooted() != 0u ) {
                h->NextState = &stBootSuccess;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* BOOT_SUCCESS: done; terminal (no NextState). */
static qSM_Status_t st_BootSuccess_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_BOOT_SUCCESS;
            s_boot.booted  = 1u;
            break;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* BOOT_RESET: count + hard-kill (PWRKEY & RST_KEY both low). */
static qSM_Status_t st_BootReset_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_BOOT_RESET;
            s_boot.retryCount++;
            Pin_PwrKey( 1u );             /* pressed (low) */
            Pin_RstKey( 1u );             /* pressed (low) */
            break;
        case SDM845_TIMEOUT:
            h->NextState = &stPmicSettle;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* PMIC_SETTLE: release keys (fix: no exit-dependency), let PMIC power-cycle. */
static qSM_Status_t st_PmicSettle_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_PMIC_SETTLE;
            Pin_PwrKey( 0u );             /* release */
            Pin_RstKey( 0u );             /* release */
            break;
        case SDM845_TIMEOUT:
            h->NextState = &stPwrPress;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* STRAP_FAIL: all released, halt (terminal). */
static qSM_Status_t st_StrapFail_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_STRAP_FAIL;
            Pin_PwrKey( 0u );             /* release */
            Pin_RstKey( 0u );             /* release */
            break;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/*============================================================================*/
/* Const timeout specifications (flash-resident)                               */
/*============================================================================*/
static const qSM_TimeoutStateDefinition_t s_toPwup[] = {
    { (qTime_t)SDM845_T_PWUP_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toHold[] = {
    { (qTime_t)SDM845_T_HOLD_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toBootChk[] = {
    { (qTime_t)SDM845_T_BOOTCHK_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toLogin[] = {
    { (qTime_t)SDM845_T_LOGIN_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toKill[] = {
    { (qTime_t)SDM845_T_KILL_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toSettle[] = {
    { (qTime_t)SDM845_T_SETTLE_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};

/*============================================================================*/
/* Public API                                                                 */
/*============================================================================*/
void Sdm845_Boot_Init( void )
{
    s_boot.stateId   = (uint8_t)SDM845_POWER_ON_WAIT;
    s_boot.retryCount = 0u;
    s_boot.booted    = 0u;

    qStateMachine_Setup( &s_boot.machine, NULL, &stPowerOnWait, NULL, NULL );

    qStateMachine_StateSubscribe( &s_boot.machine, &stPowerOnWait, NULL, st_PowerOnWait_Cb, NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stPwrPress,    NULL, st_PwrPress_Cb,    NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stBootCheck,   NULL, st_BootCheck_Cb,   NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stStage2Wait,  NULL, st_Stage2Wait_Cb,  NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stBootSuccess, NULL, st_BootSuccess_Cb, NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stBootReset,   NULL, st_BootReset_Cb,   NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stPmicSettle,  NULL, st_PmicSettle_Cb,  NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stStrapFail,   NULL, st_StrapFail_Cb,   NULL, NULL );

    qQueue_Setup( &s_boot.sigQueue, s_boot.sigStack,
                  sizeof( qSM_Signal_t ), qFLM_ArraySize( s_boot.sigStack ) );
    qStateMachine_InstallSignalQueue( &s_boot.machine, &s_boot.sigQueue );
    qStateMachine_InstallTimeoutSpec( &s_boot.machine, &s_boot.timeoutSpec );

    /* Only states holding a fixed delay get a timeout table. */
    qStateMachine_Set_StateTimeouts( &stPowerOnWait, (qSM_TimeoutStateDefinition_t *)s_toPwup,     qFLM_ArraySize( s_toPwup ) );
    qStateMachine_Set_StateTimeouts( &stPwrPress,    (qSM_TimeoutStateDefinition_t *)s_toHold,      qFLM_ArraySize( s_toHold ) );
    qStateMachine_Set_StateTimeouts( &stBootCheck,   (qSM_TimeoutStateDefinition_t *)s_toBootChk,   qFLM_ArraySize( s_toBootChk ) );
    qStateMachine_Set_StateTimeouts( &stStage2Wait,  (qSM_TimeoutStateDefinition_t *)s_toLogin,     qFLM_ArraySize( s_toLogin ) );
    qStateMachine_Set_StateTimeouts( &stBootReset,   (qSM_TimeoutStateDefinition_t *)s_toKill,      qFLM_ArraySize( s_toKill ) );
    qStateMachine_Set_StateTimeouts( &stPmicSettle,  (qSM_TimeoutStateDefinition_t *)s_toSettle,    qFLM_ArraySize( s_toSettle ) );

    (void)qOS_Add_StateMachineTask( &s_boot.task, &s_boot.machine,
                                    qLowest_Priority,
                                    (qTime_t)0.01f, qEnabled, NULL );
}

uint8_t Sdm845_Boot_IsDone( void )
{
    return s_boot.booted;
}

uint8_t Sdm845_Boot_GetState( void )
{
    return s_boot.stateId;
}

uint8_t Sdm845_Boot_GetRetries( void )
{
    return s_boot.retryCount;
}