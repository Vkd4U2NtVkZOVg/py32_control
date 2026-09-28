/**
  ******************************************************************************
  * @file    tres_cmd.c
  * @brief   TRES command policy/parser (layer 2) + host register values.
  *
  * Single-slot command mailbox: the I2C ISR fills it (when empty) and wakes
  * the disabled parser task with a notification; the task applies the
  * legality matrix of the state diagram and forwards the command to the
  * sequencer FSM. CMD_REPORT_FAIL is handled entirely here (counter only);
  * it never sends a signal, so it can never cause a state transition.
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
    uint8_t          param;   /* unused (no command carries a parameter)    */
} Cmd_Mailbox_t;

/* Private variables ---------------------------------------------------------*/
static qTask_t      s_parserTask;
static Cmd_Mailbox_t s_mailbox;
static uint8_t      s_lastErr  = TRES_ERR_NONE;
static uint8_t      s_failCnt  = 0u;   /**< CMD_REPORT_FAIL counter (RETRIES) */

/* Private function prototypes -----------------------------------------------*/
static void Parser_TaskCb(qEvent_t e);

/*============================================================================*/
/* Legality matrix (tres_sequencer_state_diagram.md).                         */
/* Returns 1 if the command was accepted, 0 if rejected (s_lastErr set).      */
/*============================================================================*/
static uint8_t Policy_Apply(uint8_t cmd, uint8_t param)
{
    (void)param;
    uint8_t st = TRES_Seq_GetState();

    /* CMD_REPORT_FAIL is legal in every state and never migrates.           */
    if ( cmd == TRES_CMD_REPORT_FAIL ) {
        s_failCnt++;
        return 1u;
    }

    if ( cmd == TRES_CMD_RESET ) {
        /* reset/reboot allowed only from RUNNING or DFU_ACTIVE              */
        if ( ( st == (uint8_t)TRES_RUNNING ) ||
             ( st == (uint8_t)TRES_DFU_ACTIVE ) ) {
            TRES_Seq_RequestReset();
            return 1u;
        }
    } else if ( cmd == TRES_CMD_ENTER_DFU ) {
        if ( st == (uint8_t)TRES_RUNNING ) {
            TRES_Seq_RequestEnterDfu();
            return 1u;
        }
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

        (void)Policy_Apply( cmd, param );
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

    if ( TRES_Seq_HostReady() != 0u )  { v |= 0x10u; }
    if ( s_lastErr != TRES_ERR_NONE )  { v |= 0x80u; }
    return v;
}

uint8_t TRES_Cmd_Retries(void)
{
    return s_failCnt;
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
    s_failCnt       = 0u;

    (void)qOS_Add_Task( &s_parserTask, Parser_TaskCb, qHigh_Priority,
                        0.0f, qPeriodic, qDisabled, NULL );
}
