/**
  ******************************************************************************
  * @file    tres_i2c_slave.c
  * @brief   TRES host interface: I2C1 slave register file (layer 1).
  *
  * PY32F030 I2C1 is an F1-style peripheral (SR1/SR2, CR2 ITEVTEN/ITBUFEN/
  * ITERREN). ADDR is cleared by the SR1->SR2 read sequence; AF (master NACK
  * at the end of a read) and STOPF are cleared explicitly. The ISR is kept
  * minimal: collect bytes / serve bytes, and hand complete commands to the
  * policy layer through its ISR-safe mailbox (tres_cmd.c).
  ******************************************************************************
  */
#include "main.h"
#include "tres_i2c_slave.h"
#include "tres_cmd.h"
#include "tres_seq.h"

/* Private define ------------------------------------------------------------*/
#define CMD_WRITE_LEN   3u      /* [magic][cmd][param]                        */

/* Private variables ---------------------------------------------------------*/
static volatile uint8_t s_regPtr;    /* register pointer, auto-increments    */
static uint8_t  s_expectPtr;         /* 1: next RX byte is the register ptr  */
static uint8_t  s_tx;                /* 1: serving a master read (TRA)       */
static uint8_t  s_cmdIdx;
static uint8_t  s_cmdBuf[ CMD_WRITE_LEN ];

/* Private function prototypes -----------------------------------------------*/
static uint8_t Reg_Read(uint8_t addr);

/*============================================================================*/
/* Register file: host-visible values, composed live                         */
/*============================================================================*/
static uint8_t Reg_Read(uint8_t addr)
{
    switch ( addr ) {
        case TRES_REG_STATUS:  return TRES_Cmd_StatusByte();
        case TRES_REG_RETRIES: return TRES_Cmd_Retries();
        case TRES_REG_PINS:    return TRES_Seq_GetPinsSnapshot();
        case TRES_REG_LASTERR: return TRES_Cmd_LastErr();
        case TRES_REG_VERSION: return TRES_IF_VERSION;
        default:               return 0u;   /* CMD is write-only          */
    }
}

/*============================================================================*/
/* Public: arm the slave interrupts (peripheral configured by Studio_I2C1_Init)*/
/*============================================================================*/
void TRES_I2C_Slave_Init(void)
{
    s_regPtr    = 0u;
    s_expectPtr = 1u;
    s_tx        = 0u;
    s_cmdIdx    = 0u;

    NVIC_SetPriority( I2C1_IRQn, 2u );
    NVIC_EnableIRQ( I2C1_IRQn );

    /* event (ADDR/STOPF) + buffer (RXNE/TXE) + error (AF/BERR/ARLO/OVR)      */
    LL_I2C_EnableIT_EVT( I2C1 );
    LL_I2C_EnableIT_BUF( I2C1 );
    LL_I2C_EnableIT_ERR( I2C1 );
    LL_I2C_Enable( I2C1 );
}

/*============================================================================*/
/* I2C1 event/error ISR (overrides the weak startup vector)                   */
/*============================================================================*/
void I2C1_IRQHandler(void)
{
    uint32_t sr1 = I2C1->SR1;

    /*--- errors -------------------------------------------------------------*/
    if ( ( sr1 & I2C_SR1_BERR ) != 0u ) { LL_I2C_ClearFlag_BERR( I2C1 ); s_expectPtr = 1u; }
    if ( ( sr1 & I2C_SR1_ARLO ) != 0u ) { LL_I2C_ClearFlag_ARLO( I2C1 ); s_expectPtr = 1u; }
    if ( ( sr1 & I2C_SR1_OVR )  != 0u ) { LL_I2C_ClearFlag_OVR( I2C1 ); }

    /*--- address match: SR1->SR2 read sequence clears ADDR ------------------*/
    if ( ( sr1 & I2C_SR1_ADDR ) != 0u ) {
        uint32_t sr2 = I2C1->SR2;

        if ( ( sr2 & I2C_SR2_TRA ) != 0u ) {
            /* master read: load the first byte now, TXE refills afterwards  */
            s_tx = 1u;
            LL_I2C_TransmitData8( I2C1, Reg_Read( s_regPtr ) );
            s_regPtr++;
        } else {
            /* master write: first data byte will be the register pointer    */
            s_tx        = 0u;
            s_expectPtr = 1u;
        }
    }

    /*--- receive ------------------------------------------------------------*/
    if ( ( sr1 & I2C_SR1_RXNE ) != 0u ) {
        uint8_t b = LL_I2C_ReceiveData8( I2C1 );

        if ( s_tx == 0u ) {
            if ( s_expectPtr != 0u ) {
                s_regPtr    = b;
                s_expectPtr = 0u;
                s_cmdIdx    = 0u;
            } else if ( s_regPtr == TRES_REG_CMD ) {
                if ( s_cmdIdx < CMD_WRITE_LEN ) {
                    s_cmdBuf[ s_cmdIdx ] = b;
                    s_cmdIdx++;
                    if ( s_cmdIdx == CMD_WRITE_LEN ) {
                        s_cmdIdx = 0u;
                        if ( s_cmdBuf[ 0 ] == TRES_CMD_MAGIC ) {
                            TRES_Cmd_PostFromISR( s_cmdBuf[ 1 ], s_cmdBuf[ 2 ] );
                        } else {
                            TRES_Cmd_PostBadMagicFromISR();
                        }
                    }
                }
            }
            /* writes to read-only registers are ignored                      */
        }
    }

    /*--- transmit: refill while the master keeps clocking -------------------*/
    if ( ( ( sr1 & I2C_SR1_TXE ) != 0u ) && ( s_tx != 0u ) ) {
        LL_I2C_TransmitData8( I2C1, Reg_Read( s_regPtr ) );
        s_regPtr++;
    }

    /*--- master NACK at end of read -----------------------------------------*/
    if ( ( sr1 & I2C_SR1_AF ) != 0u ) {
        LL_I2C_ClearFlag_AF( I2C1 );
    }

    /*--- stop condition: reset the transaction context ----------------------*/
    if ( ( sr1 & I2C_SR1_STOPF ) != 0u ) {
        LL_I2C_ClearFlag_STOP( I2C1 );
        s_expectPtr = 1u;
        s_tx        = 0u;
    }
}
