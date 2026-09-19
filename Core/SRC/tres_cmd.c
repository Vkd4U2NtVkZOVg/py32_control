/**
  ******************************************************************************
  * @file    tres_cmd.c
  * @brief   TRES command policy/parser (layer 2) + host register values.
  *
  * Single-slot command mailbox: the I2C ISR fills it (when empty) and wakes
  * the disabled parser task with a notification; the task applies the
  * legality matrix of the state diagram and forwards the command to the
  * sequencer FSM. A command arriving while the slot is busy is dropped and
  * shows up as a rejected command in STATUS (the host is expected to poll).
  ******************************************************************************
  */
#include "main.h"
#include "QuarkTS.h"
#include "tres_cmd.h"
#include "tres_seq.h"

/* Private typedef -----------------------------------------------------------*/
typedef struct {
    volatile uint8_t valid;
    uint8_t          cmd;
    uint8_t          param;
} Cmd_Mailbox_t;

/* Private variables ---------------------------------------------------------*/
static qTask_t      s_parserTask;
static Cmd_Mailbox_t s_mailbox;
static uint8_t      s_lastErr = TRES_ERR_NONE;

/* Private function prototypes -----------------------------------------------*/
static void Parser_TaskCb(qEvent_t e);

/*============================================================================*/
/* Legality matrix (tres_sequencer_state_diagram.md).                         */
/* Returns 1 if the command was forwarded, 0 if rejected (s_lastErr set).     */
/*============================================================================*/
static uint8_t Policy_Apply(uint8_t cmd, uint8_t param)
{
    uint8_t st = TRES_Seq_GetState();

    if ( TRES_Seq_IsBusy() != 0u ) {
        s_lastErr = TRES_ERR_BUSY;
        return 0u;
    }

    switch ( st ) {
        case TRES_ST_OFF:
        case TRES_ST_FAULT:
            /* only (re)boot / power-off commands make sense here             */
            if ( ( cmd == TRES_CMD_PWR_ON_NORMAL ) || ( cmd == TRES_CMD_PWR_ON_DFU ) ) {
                if ( param > (uint8_t)TRES_MODE_DFU ) { break; }
                TRES_Seq_RequestPowerOn( ( param == (uint8_t)TRES_MODE_DFU )
                                         ? TRES_MODE_DFU : TRES_MODE_NORMAL );
                return 1u;
            }
            if ( cmd == TRES_CMD_POWER_OFF ) {
                if ( st == TRES_ST_FAULT ) { TRES_Seq_RequestPowerOff(); }
                /* in OFF already: accepted no-op                              */
                return 1u;
            }
            break;

        case TRES_ST_PWR_UP:
        case TRES_ST_BOOT_SETUP:
        case TRES_ST_BOOT_WAIT_NORMAL:
        case TRES_ST_BOOT_WAIT_DFU:
            /* boot sequence in progress: only an abort is allowed            */
            if ( cmd == TRES_CMD_POWER_OFF ) {
                TRES_Seq_RequestPowerOff();
                return 1u;
            }
            break;

        case TRES_ST_RUNNING:
            switch ( cmd ) {
                case TRES_CMD_PWR_ON_DFU:
                    TRES_Seq_RequestPowerOn( TRES_MODE_DFU );
                    return 1u;
                case TRES_CMD_POWER_OFF:
                    TRES_Seq_RequestPowerOff();
                    return 1u;
                case TRES_CMD_RESET:
                    TRES_Seq_RequestReset();
                    return 1u;
                case TRES_CMD_REBOOT_NORMAL:
                    TRES_Seq_RequestRebootNormal();
                    return 1u;
                case TRES_CMD_REPORT_FAIL:
                    TRES_Seq_RequestFailReport();
                    return 1u;
                default:               /* PWR_ON_NORMAL while running        */
                    break;
            }
            break;

        case TRES_ST_DFU_ACTIVE:
            switch ( cmd ) {
                case TRES_CMD_POWER_OFF:
                    TRES_Seq_RequestPowerOff();
                    return 1u;
                case TRES_CMD_RESET:
                    TRES_Seq_RequestReset();       /* BOOT0 still 1: DFU again */
                    return 1u;
                case TRES_CMD_REBOOT_NORMAL:
                    TRES_Seq_RequestRebootNormal();
                    return 1u;
                case TRES_CMD_REPORT_FAIL:
                    TRES_Seq_RequestFailReport();
                    return 1u;
                default:               /* PWR_ON_* while DFU session active  */
                    break;
            }
            break;

        default:                       /* PULSE_RESET: transient, reject all */
            break;
    }

    s_lastErr = TRES_ERR_ILLEGAL;
    return 0u;
}

/*============================================================================*/
/* Parser task: disabled; runs once per notification.                         */
/*============================================================================*/
static void Parser_TaskCb(qEvent_t e)
{
    (void)e;

    if ( s_mailbox.valid != 0u ) {
        uint8_t cmd   = s_mailbox.cmd;
        uint8_t param = s_mailbox.param;
        s_mailbox.valid = 0u;          /* ISR may refill from here on        */

        if ( param > (uint8_t)TRES_MODE_DFU ) {
            s_lastErr = TRES_ERR_PARAM;
        } else {
            (void)Policy_Apply( cmd, param );
        }
    }
}

/*============================================================================*/
/* ISR-side interface                                                         */
/*============================================================================*/
void TRES_Cmd_PostFromISR(uint8_t cmd, uint8_t param)
{
    if ( s_mailbox.valid != 0u ) {
        return;                        /* dropped: host polls STATUS/retries */
    }
    s_mailbox.cmd   = cmd;
    s_mailbox.param = param;
    s_mailbox.valid = 1u;              /* written last: release to the task  */
    (void)qTask_Notification_Send( &s_parserTask, NULL );
}

void TRES_Cmd_PostBadMagicFromISR(void)
{
    s_lastErr = TRES_ERR_MAGIC;
}

/*============================================================================*/
/* Register values for the I2C layer                                          */
/*============================================================================*/
uint8_t TRES_Cmd_StatusByte(void)
{
    uint8_t v = (uint8_t)( TRES_Seq_GetState() & 0x0Fu );

    if ( TRES_Seq_HostReady() != 0u )   { v |= 0x10u; }
    if ( TRES_Seq_FaultLatched() != 0u ){ v |= 0x20u; }
    if ( TRES_Seq_IsBusy() != 0u )      { v |= 0x40u; }
    if ( s_lastErr != TRES_ERR_NONE )   { v |= 0x80u; }
    return v;
}

uint8_t TRES_Cmd_Retries(void)
{
    return TRES_Seq_GetRetries();
}

uint8_t TRES_Cmd_LastErr(void)
{
    return s_lastErr;
}

/*============================================================================*/
/* Public: create the disabled parser task (woken by the I2C ISR)             */
/*============================================================================*/
void TRES_Cmd_Init(void)
{
    s_mailbox.valid = 0u;
    s_mailbox.cmd   = 0u;
    s_mailbox.param = 0u;
    s_lastErr       = TRES_ERR_NONE;

    (void)qOS_Add_Task( &s_parserTask, Parser_TaskCb, qHigh_Priority,
                        0.0f, qPeriodic, qDisabled, NULL );
}
