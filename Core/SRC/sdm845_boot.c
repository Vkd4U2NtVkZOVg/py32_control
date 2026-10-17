/**
 * @file sdm845_boot.c
 * @brief SDM845 boot/reset/strap state machine (QuarkTS).
 *
 * State machine (polled every 10 ms), five-stage boot detection:
 *
 *     [*] --> THERMAL_PRECHECK --ambient ok--> POWER_ON_WAIT --t_pwup(100ms)--> PWR_PRESS
 *     PWR_PRESS --t_hold(3s)--> BOOT_CHECK
 *     BOOT_CHECK --stage1 "Boot Config @"--> STAGE2_WAIT
 *     BOOT_CHECK --15s 无响应 & retry<MAX--> BOOT_RESET
 *     BOOT_CHECK --15s 无响应 & retry>=MAX--> STRAP_FAIL (terminal)
 *     STAGE2_WAIT --stage2 "login" + idle--> STAGE3_WAIT
 *     STAGE2_WAIT --t_login(60s) 无响应--> STRAP_FAIL (terminal, 不复位)
 *     STAGE3_WAIT --entry: TX->PY32 (PF3 TX-SW) + TX DMA "comma\r\n", "Password:"--> STAGE4_WAIT
 *     STAGE4_WAIT --entry: TX DMA "comma\r\n" (password), "Welcome to AGNOS"--> STAGE5_WAIT
 *     STAGE5_WAIT --entry: settle 10s + TX DMA "\r\n\r\namixer set PCM 100%\r\n\r\n\r\n", "Front Left: Playback"--> BOOT_SUCCESS
 *     STAGE2/3/4/5_WAIT --超时--> STRAP_FAIL (terminal, 不复位; 硬复位仅保留于 BOOT_CHECK)
 *     BOOT_RESET --t_kill(15s)--> PMIC_SETTLE
 *     PMIC_SETTLE --t_settle(3s)--> PWR_PRESS (retry loop)
 *
 * THERMAL_PRECHECK (first state): before powering the SoC/TEC, poll the AHT20
 * ambient temperature. If it exceeds SDM845_PRECHECK_TEMP_MILLIC (70 °C) run the
 * fan at full speed until it falls to SDM845_PRECHECK_CLEAR_MILLIC (60 °C), then
 * boot. An already-cool enclosure (<= 70 °C) boots straight away; an invalid
 * reading keeps waiting; after SDM845_PRECHECK_MAX_S the check fails open so a
 * stuck sensor cannot permanently block boot. Passing it latches thermalReady,
 * which gates the TEC control task (TecControl).
 *
 * STAGE3/4/5_WAIT drive the post-login command chain: STAGE3 entry routes the
 * SoC RX line to the PY32 TX via the PF3 TX switch (USART2_SetTxSource(PY32)),
 * re-arms the stage 3/4/5 matchers (Sdm845_Detect_ResetStage345) and queues the
 * command over non-blocking TX DMA (USART2_DMA_Send). Each stage advances on
 * its own RX banner (account echo / "Welcome to AGNOS" / "Front Left:
 * Playback"). BOOT_SUCCESS / BOOT_RESET / STRAP_FAIL hand the line back to the
 * CH343 (USART2_SetTxSource(CH343)) — the PY32-TX window is exactly stage3..5.
 *
 * GPIO: PWRKEY = MIX3_BOOTKEY (PB7), RST_KEY = MIX3_RST (PB8), low = pressed.
 * Stage 1 ("Boot Config @") proves the bootloader is alive; stage 2
 * ("login") proves Linux user space came up; stages 3-5 prove the PY32 can
 * actually drive the SoC shell. A loader without a login points at a
 * kernel/init hang -> targeted retry.
 *
 * Key constraints honored:
 *   - Never set h->NextState in a QSM_SIGNAL_ENTRY handler (engine ignores it).
 *   - Decision states (BOOT_CHECK, STAGE2/3/4/5_WAIT, THERMAL_PRECHECK) read
 *     the detector/sensor in the periodic-poll (QSM_SIGNAL_NONE) branch and
 *     set NextState there; fixed delays use const timeout tables. Success is
 *     checked BEFORE the stage timeout, so a late-arriving banner is not
 *     spuriously treated as failure.
 *   - USART2_DMA_Send is non-blocking and rejects a second concurrent send;
 *     the <20 B commands finish in <2 ms, far below the 10 ms poll period.
 */
#include "main.h"
#include "QuarkTS.h"
#include "gpio.h"
#include "sdm845_boot.h"
#include "sdm845_detect.h"
#include "fan.h"
#include "aht20_task.h"
#include "usart.h"
#include "usart2_dma.h"
#include "qport_py32.h"

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
#define SDM845_T_STAGE345_S (15.0f)    /* stage 3/4/5: command echo wait */
#define SDM845_T_STAGE5_SETTLE_MS (10000u)  /* stage 5: wait for the shell to
                                        * finish motd/prompt setup after
                                        * "Welcome to AGNOS" before typing   */
#define SDM845_T_KILL_S     (15.0f)    /* hard-kill (PWR+RST low) pulse  */
#define SDM845_T_SETTLE_S   (3.0f)     /* PMIC power-cycle settle        */
#define SDM845_BOOT_MAX_RETRIES  (3u)

/* Post-login command chain (sent once per STAGE entry over TX DMA). */
static const char SDM845_CMD_ACCOUNT[] = "comma\r\n";                 /* stage 3 */
static const char SDM845_CMD_PASSWORD[] = "comma\r\n";                /* stage 4 */
static const char SDM845_CMD_AMIXER[] = "\r\n\r\namixer set PCM 100%\r\n\r\n\r\n";    /* stage 5 */

/*============================================================================*/
/* TX queue: one command pending at a time. A send rejected by TxBusy() is    */
/* retried from the poll branch instead of being dropped (a dropped command   */
/* would hang the stage until its 15 s timeout for nothing). A pending        */
/* command also waits out its settle time before the FIRST transmit: the      */
/* password must not land while login(1) is still switching the tty from      */
/* username-echo to password-noecho (it flushes the input queue on the way).  */
/*============================================================================*/
static const char *s_txPending;
static uint32_t    s_txPendingLen;
static uint32_t    s_txArmTick;     /* QOS_Tick_Get() when the cmd was queued */
static uint32_t    s_txSettleMs;    /* delay before the first transmit        */

static void sdm845_stage_send( const char *cmd, uint32_t len, uint32_t settle_ms )
{
    s_txPending    = cmd;
    s_txPendingLen = len;
    s_txSettleMs   = settle_ms;
    s_txArmTick    = QOS_Tick_Get();
    if ( ( settle_ms == 0u ) &&
         ( USART2_DMA_Send( (const uint8_t *)cmd, len ) == 0u ) ) {
        s_txPending = NULL;              /* accepted: DMA owns it now        */
    }
}

static void sdm845_stage_send_retry( void )
{
    if ( ( s_txPending != NULL ) &&
         ( ( QOS_Tick_Get() - s_txArmTick ) >= s_txSettleMs ) &&
         ( USART2_DMA_Send( (const uint8_t *)s_txPending,
                            s_txPendingLen ) == 0u ) ) {
        s_txPending = NULL;              /* went out on this poll            */
    }
}

/* Thermal precheck: before powering the SoC/TEC, cool the enclosure if the
 * AHT20 ambient temperature is too high. Fan runs full speed until the temp
 * falls below the clear threshold, then boot proceeds. */
#define SDM845_PRECHECK_TEMP_MILLIC   (70000)   /* 70 °C: too hot to boot  */
#define SDM845_PRECHECK_CLEAR_MILLIC  (60000)   /* 60 °C: cool enough      */
#define SDM845_PRECHECK_VALID_MIN_MILLIC (-20000)
#define SDM845_PRECHECK_VALID_MAX_MILLIC (120000)
#define SDM845_PRECHECK_MAX_S         (300.0f)  /* give up cooling after 5 min */
#define SDM845_PRECHECK_POLL_MS       (1000u)   /* AHT20 samples every 10 s */

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
    uint8_t           thermalReady;     /* THERMAL_PRECHECK passed latch */
    uint8_t           precheckCooling;  /* 1 = actively cooling (hysteresis) */
    uint16_t          precheckPolls;    /* THERMAL_PRECHECK poll counter   */
} Sdm845_Boot_t;

static Sdm845_Boot_t s_boot;

static qSM_State_t stThermalPrecheck, stPowerOnWait, stPwrPress, stBootCheck, stStage2Wait,
                   stStage3Wait, stStage4Wait, stStage5Wait,
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
/* THERMAL_PRECHECK: first state, checks AHT20 ambient before powering SoC/TEC.
 * Ambient > 70°C -> fan full speed and hold until it drops to <= 60°C (hysteresis
 * via the cooling flag), then boot. Ambient already <= 70°C -> boot straight
 * away (no need to cool). An invalid reading (AHT20 not measured yet, returns
 * INT32_MIN) keeps waiting. After SDM845_PRECHECK_MAX_S the check fails open so
 * a dead sensor cannot brick the device. */
static qSM_Status_t st_ThermalPrecheck_Cb( qSM_Handler_t h )
{
    int32_t ambient_mc;
    uint8_t valid;
    uint8_t proceed;

    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_THERMAL_PRECHECK;
            s_boot.precheckPolls   = 0u;
            s_boot.precheckCooling = 0u;
            Pin_PwrKey( 0u );
            Pin_RstKey( 0u );
            /* The precheck needs the fan at full speed to cool the enclosure
             * BEFORE the SoC/TEC start. The PY32 owns the PWM exclusively,
             * so enable it before the timeout branch writes the fan duty.
             * The TEC loop enables it again on first TEC drive as a backstop. */
            Fan_SetEnabled( 1u );
            break;
        case SDM845_TIMEOUT:
            ambient_mc = AHT20_Get_LastTempMilliCelsius();
            valid = ( ambient_mc >= SDM845_PRECHECK_VALID_MIN_MILLIC ) &&
                    ( ambient_mc <= SDM845_PRECHECK_VALID_MAX_MILLIC );
            s_boot.precheckPolls++;

            proceed = 0u;
            if ( valid && ( ambient_mc > SDM845_PRECHECK_TEMP_MILLIC ) ) {
                s_boot.precheckCooling = 1u;   /* too hot: begin/keep cooling */
            }
            if ( s_boot.precheckCooling != 0u ) {
                proceed = ( valid &&
                            ( ambient_mc <= SDM845_PRECHECK_CLEAR_MILLIC ) ) ? 1u : 0u;
            } else {
                /* never exceeded the hot threshold: boot when not hot */
                proceed = ( valid &&
                            ( ambient_mc <= SDM845_PRECHECK_TEMP_MILLIC ) ) ? 1u : 0u;
            }

            if ( s_boot.precheckPolls >=
                 (uint16_t)( SDM845_PRECHECK_MAX_S * 1000.0f /
                             (float)SDM845_PRECHECK_POLL_MS ) ) {
                proceed = 1u;   /* fail open: do not block boot on a stuck sensor */
            }

            if ( proceed != 0u ) {
                s_boot.thermalReady = 1u;
                Fan_SetDutyPercent( 0u );     /* cooled: hand fan to TEC loop */
                h->NextState = &stPowerOnWait;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            if ( s_boot.precheckCooling != 0u ) {
                Fan_SetDutyPercent( 100u );   /* still hot: keep cooling */
            }
            break;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

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
            /* Success check first even on the timeout delivery: SOM_BOOTED
             * is a pin level that can rise in the same gap the timeout
             * expires - the poll-only check would lose that race and
             * re-power a just-booted SoC. */
            if ( LL_GPIO_IsInputPinSet( GPIOB, SOM_BOOTED_Pin ) != 0u ) {
                h->NextState = &stBootSuccess;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
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

/* PWR_PRESS: assert PWRKEY low to power the SoC on. No detector reset here:
 * the SoC rails are always on, so the loader banner legitimately arrives
 * during THERMAL_PRECHECK/POWER_ON_WAIT and that latch must survive into
 * BOOT_CHECK. Retries re-sync at BOOT_RESET instead. */
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

/* BOOT_CHECK: keys released on entry (fix: no exit-dependency), then the
 * detector is checked on BOTH the poll and the timeout delivery (success
 * priority - the RX-ISR latch and the timeout can land in the same window);
 * only an expired timeout without the latch drives the failure branch
 * (retry or strap-fail). */
static qSM_Status_t st_BootCheck_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_BOOT_CHECK;
            Pin_PwrKey( 0u );             /* release */
            Pin_RstKey( 0u );             /* release */
            break;
        case SDM845_TIMEOUT:
            /* Success check first even on the timeout delivery: the stage-1
             * latch comes from the RX ISR and can fire in the same gap the
             * timeout expires - a poll-only check would lose that race and
             * reset a just-booted loader. */
            if ( Sdm845_Detect_IsStage1Seen() != 0u ) {
                h->NextState = &stStage2Wait;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
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

/* STAGE2_WAIT: loader proved stage 1; now wait for the "login" prompt and a
 * quiet line. Stage 2 + idle gates entry into the login command chain
 * (STAGE3..5); it no longer completes the boot by itself. */
static qSM_Status_t st_Stage2Wait_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_STAGE2_WAIT;
            break;
        case SDM845_TIMEOUT:
            /* Success check first even on the timeout delivery (same race as
             * BOOT_CHECK): the stage2+idle latch can land with it. */
            if ( Sdm845_Detect_IsBooted() != 0u ) {
                h->NextState = &stStage3Wait;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            /* Loader up but no login after t_login: kernel/init hang. The
             * hard power-cycle budget is spent in BOOT_CHECK only; once the
             * loader banner matched, no more resets - halt for recovery. */
            h->NextState = &stStrapFail;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            /* Periodic poll: stage2+idle (stable login prompt) takes priority
             * over the 60 s timeout; BOOT_SUCCESS now comes only from
             * STAGE5_WAIT (amixer answer). */
            if ( Sdm845_Detect_IsBooted() != 0u ) {
                h->NextState = &stStage3Wait;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* STAGE3_WAIT: login prompt answered. Entry routes the SoC RX to the PY32 TX
 * via the PF3 TX switch, re-arms the stage 3/4/5 matchers so each retry pass
 * starts clean, and queues the account over non-blocking TX DMA. The stage
 * advances on the getty "Password:" prompt - proof that the account was
 * accepted AND login has switched the tty to password-noecho, so sending the
 * password next can never land in the username->password input flush. */
static qSM_Status_t st_Stage3Wait_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_STAGE3_WAIT;
            USART2_SetTxSource( USART2_TXSRC_PY32 );  /* isolate CH343, PA0 sends */
            Sdm845_Detect_ResetStage345();        /* fresh matchers per pass */
            sdm845_stage_send( SDM845_CMD_ACCOUNT,
                               sizeof( SDM845_CMD_ACCOUNT ) - 1u, 0u );
            break;
        case SDM845_TIMEOUT:
            /* Success check first even on the timeout delivery: the
             * "Password:" latch can land with it. */
            if ( Sdm845_Detect_IsStage3Seen() != 0u ) {
                h->NextState = &stStage4Wait;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            /* No "Password:" prompt: login never accepted the account. The
             * hard power-cycle budget is spent in BOOT_CHECK only - once past
             * the loader banner, timeout halts instead of resetting. */
            h->NextState = &stStrapFail;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            sdm845_stage_send_retry();    /* re-send if the queue was busy   */
            /* Periodic poll: prompt takes priority over the 15 s timeout. */
            if ( Sdm845_Detect_IsStage3Seen() != 0u ) {
                h->NextState = &stStage4Wait;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* STAGE4_WAIT: password prompt was seen (stage 3 gate), so login is already
 * in the noecho password read - the password goes out immediately on entry
 * with no settle delay. The stage advances on the post-login banner
 * "Welcome to AGNOS". */
static qSM_Status_t st_Stage4Wait_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_STAGE4_WAIT;
            sdm845_stage_send( SDM845_CMD_PASSWORD,
                               sizeof( SDM845_CMD_PASSWORD ) - 1u, 0u );
            break;
        case SDM845_TIMEOUT:
            /* Success check first even on the timeout delivery: the
             * "Welcome to AGNOS" latch can land with it. */
            if ( Sdm845_Detect_IsStage4Seen() != 0u ) {
                h->NextState = &stStage5Wait;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            /* No "Welcome to AGNOS": the password was rejected or getty died.
             * Same policy as STAGE2/3: no reset past the loader banner, halt. */
            h->NextState = &stStrapFail;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            sdm845_stage_send_retry();
            if ( Sdm845_Detect_IsStage4Seen() != 0u ) {
                h->NextState = &stStage5Wait;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* STAGE5_WAIT: first real command. Entry queues "amixer set PCM 100%" over
 * TX DMA; the stage advances on the mixer reply "Front Left: Playback". */
static qSM_Status_t st_Stage5Wait_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_STAGE5_WAIT;
            sdm845_stage_send( SDM845_CMD_AMIXER,
                               sizeof( SDM845_CMD_AMIXER ) - 1u,
                               SDM845_T_STAGE5_SETTLE_MS );
            break;
        case SDM845_TIMEOUT:
            /* Success check first even on the timeout delivery: the amixer
             * reply latch can land with it. */
            if ( Sdm845_Detect_IsStage5Seen() != 0u ) {
                h->NextState = &stBootSuccess;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            /* STAGE5 never matched the amixer reply. By now the SoC is fully
             * past hardware bring-up (loader + kernel + userspace login), so a
             * PMIC hard-cycle (BOOT_RESET) would not fix a userspace-side miss
             * and could disturb a half-booted system. Halt (STRAP_FAIL) for
             * external recovery instead of resetting the 845. */
            h->NextState = &stStrapFail;
            return qSM_STATUS_SIGNAL_HANDLED;
        default:
            sdm845_stage_send_retry();
            if ( Sdm845_Detect_IsStage5Seen() != 0u ) {
                h->NextState = &stBootSuccess;
                return qSM_STATUS_SIGNAL_HANDLED;
            }
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* BOOT_SUCCESS: done; terminal. Entry hands the SoC RX line back to the
 * CH343 (PF3 TX switch) so the host console works normally after boot. */
static qSM_Status_t st_BootSuccess_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_BOOT_SUCCESS;
            s_boot.booted  = 1u;
            USART2_SetTxSource( USART2_TXSRC_CH343 );  /* host owns the line */
            break;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/* BOOT_RESET: count + hard-kill (PWRKEY & RST_KEY both low). Entry also hands
 * the SoC RX line back to the CH343 (PF3 TX switch) and clears every detector
 * latch BEFORE the PMIC cycle: the re-booting SoC prints a fresh loader
 * banner during the kill/settle window and that (not the stale one) must
 * re-latch for the next BOOT_CHECK. */
static qSM_Status_t st_BootReset_Cb( qSM_Handler_t h )
{
    switch ( h->Signal ) {
        case QSM_SIGNAL_ENTRY:
            s_boot.stateId = (uint8_t)SDM845_BOOT_RESET;
            s_boot.retryCount++;
            USART2_SetTxSource( USART2_TXSRC_CH343 );  /* end the TX window */
            Sdm845_Detect_ResetAll();     /* re-sync before the fresh banner */
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
            USART2_SetTxSource( USART2_TXSRC_CH343 );  /* end the TX window */
            /* Terminal state: nothing else will ever drive the fan again.
             * Hold full airflow through the halt state. */
            Fan_SetEnabled( 1u );
            Fan_SetDutyPercent( 100u );
            break;
        default:
            break;
    }
    return qSM_STATUS_EXIT_SUCCESS;
}

/*============================================================================*/
/* Const timeout specifications (flash-resident)                               */
/*============================================================================*/
static const qSM_TimeoutStateDefinition_t s_toThermalPrecheck[] = {
    { (qTime_t)( (float)SDM845_PRECHECK_POLL_MS / 1000.0f ),
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT |
      QSM_TSOPT_PERIODIC },   /* periodic: keep polling AHT20 while too hot /
                                 sensor invalid until proceed or fail-open   */
};
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
static const qSM_TimeoutStateDefinition_t s_toStage3[] = {
    { (qTime_t)SDM845_T_STAGE345_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toStage4[] = {
    { (qTime_t)SDM845_T_STAGE345_S,
      QSM_TSOPT_INDEX(0) | QSM_TSOPT_SET_ENTRY | QSM_TSOPT_RST_EXIT },
};
static const qSM_TimeoutStateDefinition_t s_toStage5[] = {
    { (qTime_t)SDM845_T_STAGE345_S,
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
    s_boot.stateId      = (uint8_t)SDM845_THERMAL_PRECHECK;
    s_boot.retryCount   = 0u;
    s_boot.booted       = 0u;
    s_boot.thermalReady = 0u;

    qStateMachine_Setup( &s_boot.machine, NULL, &stThermalPrecheck, NULL, NULL );

    qStateMachine_StateSubscribe( &s_boot.machine, &stThermalPrecheck, NULL, st_ThermalPrecheck_Cb, NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stPowerOnWait, NULL, st_PowerOnWait_Cb, NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stPwrPress,    NULL, st_PwrPress_Cb,    NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stBootCheck,   NULL, st_BootCheck_Cb,   NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stStage2Wait,  NULL, st_Stage2Wait_Cb,  NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stStage3Wait,  NULL, st_Stage3Wait_Cb,  NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stStage4Wait,  NULL, st_Stage4Wait_Cb,  NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stStage5Wait,  NULL, st_Stage5Wait_Cb,  NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stBootSuccess, NULL, st_BootSuccess_Cb, NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stBootReset,   NULL, st_BootReset_Cb,   NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stPmicSettle,  NULL, st_PmicSettle_Cb,  NULL, NULL );
    qStateMachine_StateSubscribe( &s_boot.machine, &stStrapFail,   NULL, st_StrapFail_Cb,   NULL, NULL );

    qQueue_Setup( &s_boot.sigQueue, s_boot.sigStack,
                  sizeof( qSM_Signal_t ), qFLM_ArraySize( s_boot.sigStack ) );
    qStateMachine_InstallSignalQueue( &s_boot.machine, &s_boot.sigQueue );
    qStateMachine_InstallTimeoutSpec( &s_boot.machine, &s_boot.timeoutSpec );

    /* Only states holding a fixed delay get a timeout table. (12 states are
     * subscribed; the 7 delay-gated ones are wired below.) */
    qStateMachine_Set_StateTimeouts( &stThermalPrecheck, (qSM_TimeoutStateDefinition_t *)s_toThermalPrecheck, qFLM_ArraySize( s_toThermalPrecheck ) );
    qStateMachine_Set_StateTimeouts( &stPowerOnWait, (qSM_TimeoutStateDefinition_t *)s_toPwup,     qFLM_ArraySize( s_toPwup ) );
    qStateMachine_Set_StateTimeouts( &stPwrPress,    (qSM_TimeoutStateDefinition_t *)s_toHold,      qFLM_ArraySize( s_toHold ) );
    qStateMachine_Set_StateTimeouts( &stBootCheck,   (qSM_TimeoutStateDefinition_t *)s_toBootChk,   qFLM_ArraySize( s_toBootChk ) );
    qStateMachine_Set_StateTimeouts( &stStage2Wait,  (qSM_TimeoutStateDefinition_t *)s_toLogin,     qFLM_ArraySize( s_toLogin ) );
    qStateMachine_Set_StateTimeouts( &stStage3Wait,  (qSM_TimeoutStateDefinition_t *)s_toStage3,    qFLM_ArraySize( s_toStage3 ) );
    qStateMachine_Set_StateTimeouts( &stStage4Wait,  (qSM_TimeoutStateDefinition_t *)s_toStage4,    qFLM_ArraySize( s_toStage4 ) );
    qStateMachine_Set_StateTimeouts( &stStage5Wait,  (qSM_TimeoutStateDefinition_t *)s_toStage5,    qFLM_ArraySize( s_toStage5 ) );
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

uint8_t Sdm845_Boot_IsThermalReady( void )
{
    return s_boot.thermalReady;
}

uint8_t Sdm845_Boot_GetState( void )
{
    return s_boot.stateId;
}

uint8_t Sdm845_Boot_GetRetries( void )
{
    return s_boot.retryCount;
}
