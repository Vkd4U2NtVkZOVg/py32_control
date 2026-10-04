/**
 * @file usart2_dma.c
 * @brief USART2 (PA0/PA1) DMA receive ring (DMA1_Channel1) + non-blocking TX
 *        (DMA1_Channel2). See usart2_dma.h for the request-mapping codes.
 */
#include "usart2_dma.h"
#include "usart.h"
#include "main.h"

/*---------------------------------------------------------------------------*/
/* Local state                                                                 */
/*---------------------------------------------------------------------------*/
/* Circular DMA receive buffer. DMA writes here continuously. */
static uint8_t  s_rx_buf[ USART2_DMA_RX_BUF_LEN ];
/* Read cursor: index of the next unread byte (consumed by USART2_DMA_Read). */
static volatile uint32_t s_mark;

/*---------------------------------------------------------------------------*/
/* DMA write position: how many bytes the DMA has written into the ring.      */
/* In circular mode the remaining count runs 0..BUF_LEN, so the written count */
/* is BUF_LEN - remaining, i.e. the next write index.                         */
/*---------------------------------------------------------------------------*/
static uint32_t usart2_dma_write_pos( void )
{
    return USART2_DMA_RX_BUF_LEN -
           LL_DMA_GetDataLength( DMA1, LL_DMA_CHANNEL_1 );
}

/*---------------------------------------------------------------------------*/
/* Public API                                                                 */
/*---------------------------------------------------------------------------*/
void USART2_DMA_Start( void )
{
    /* 0) DMA1 peripheral clock MUST be enabled before touching any of its
     * registers: without it every register write below is silently ignored
     * (the channel never enables, CNDTR never counts), which shows up as
     * "RX line has data but the DMA receives nothing". */
    LL_AHB1_GRP1_EnableClock( LL_AHB1_GRP1_PERIPH_DMA1 );

    /* 1) Route the two channels to their USART2 requests via SYSCFG_CFGR3.
     * DMA1_MAP / DMA2_MAP are independent 5-bit fields (one per channel):
     * CH1 -> USART2_RX (01000), CH2 -> USART2_TX (00111). Without the CH2
     * write the TX channel stays on its reset source (ADC) and never sees a
     * request: the transfer arms, moves zero bytes and never completes. */
    MODIFY_REG( SYSCFG->CFGR3, SYSCFG_CFGR3_DMA1_MAP_Msk,
                (0x08UL << SYSCFG_CFGR3_DMA1_MAP_Pos) );   /* 01000 = USART2_RX */
    MODIFY_REG( SYSCFG->CFGR3, SYSCFG_CFGR3_DMA2_MAP_Msk,
                (0x07UL << SYSCFG_CFGR3_DMA2_MAP_Pos) );   /* 00111 = USART2_TX */

    /* 2) DMA1_Channel1: peripheral->memory, circular, byte/byte, mem incr. */
    LL_DMA_SetDataTransferDirection( DMA1, LL_DMA_CHANNEL_1,
                                     LL_DMA_DIRECTION_PERIPH_TO_MEMORY );
    LL_DMA_SetMode( DMA1, LL_DMA_CHANNEL_1, LL_DMA_MODE_CIRCULAR );
    LL_DMA_SetPeriphIncMode( DMA1, LL_DMA_CHANNEL_1, LL_DMA_PERIPH_NOINCREMENT );
    LL_DMA_SetMemoryIncMode( DMA1, LL_DMA_CHANNEL_1, LL_DMA_MEMORY_INCREMENT );
    LL_DMA_SetPeriphSize( DMA1, LL_DMA_CHANNEL_1, LL_DMA_PDATAALIGN_BYTE );
    LL_DMA_SetMemorySize( DMA1, LL_DMA_CHANNEL_1, LL_DMA_MDATAALIGN_BYTE );
    LL_DMA_SetChannelPriorityLevel( DMA1, LL_DMA_CHANNEL_1,
                                    LL_DMA_PRIORITY_MEDIUM );

    /* 3) Source = USART2 receive data register; destination = our buffer. */
    LL_DMA_SetPeriphAddress( DMA1, LL_DMA_CHANNEL_1,
                             (uint32_t)LL_USART_DMA_GetRegAddr( USART2 ) );
    LL_DMA_SetMemoryAddress( DMA1, LL_DMA_CHANNEL_1,
                             (uint32_t)s_rx_buf );
    LL_DMA_SetDataLength( DMA1, LL_DMA_CHANNEL_1, USART2_DMA_RX_BUF_LEN );

    /* 4) Let USART2 request DMA on RX, then enable the channel.
     * No IDLE interrupt is used anywhere: frame/content decisions belong to
     * the consumer (sdm845_detect), which polls USART2_DMA_Read(). */
    LL_USART_EnableDMAReq_RX( USART2 );
    LL_DMA_EnableChannel( DMA1, LL_DMA_CHANNEL_1 );

    s_mark = 0u;
}

void USART2_DMA_Stop( void )
{
    LL_DMA_DisableChannel( DMA1, LL_DMA_CHANNEL_1 );
    LL_USART_DisableDMAReq_RX( USART2 );
}

size_t USART2_DMA_Read( uint8_t *out, size_t max_len )
{
    uint32_t write = usart2_dma_write_pos();
    uint32_t available;
    uint32_t i;

    /* New bytes are [s_mark, write) in ring order (wrapping at BUF_LEN). */
    available = ( write >= s_mark )
                ? ( write - s_mark )
                : ( USART2_DMA_RX_BUF_LEN - s_mark + write );

    if ( available > max_len ) {
        available = (uint32_t)max_len;
    }

    for ( i = 0u ; i < available ; ++i ) {
        out[ i ] = s_rx_buf[ s_mark ];
        s_mark = ( s_mark + 1u ) % USART2_DMA_RX_BUF_LEN;
    }
    return (size_t)available;
}

/*---------------------------------------------------------------------------*/
/* TX: DMA1_Channel2 -> USART2_TX, one non-circular shot per Send() call.     */
/* The CH2 request route (DMA2_MAP = USART2_TX) is programmed once in         */
/* USART2_DMA_Start(); this function only re-arms the transfer.               */
/*---------------------------------------------------------------------------*/
uint8_t USART2_DMA_Send( const uint8_t *data, size_t len )
{
    if ( ( data == NULL ) || ( len == 0u ) ) {
        return 0u;                       /* nothing to send: not an error   */
    }
    if ( USART2_DMA_TxBusy() != 0u ) {
        return 1u;                       /* previous transfer still running */
    }
    if ( len > 0xFFFFu ) {
        len = 0xFFFFu;                   /* DMA_NbData register limit       */
    }

    LL_DMA_SetDataTransferDirection( DMA1, LL_DMA_CHANNEL_2,
                                     LL_DMA_DIRECTION_MEMORY_TO_PERIPH );
    LL_DMA_SetMode( DMA1, LL_DMA_CHANNEL_2, LL_DMA_MODE_NORMAL );
    LL_DMA_SetPeriphIncMode( DMA1, LL_DMA_CHANNEL_2, LL_DMA_PERIPH_NOINCREMENT );
    LL_DMA_SetMemoryIncMode( DMA1, LL_DMA_CHANNEL_2, LL_DMA_MEMORY_INCREMENT );
    LL_DMA_SetPeriphSize( DMA1, LL_DMA_CHANNEL_2, LL_DMA_PDATAALIGN_BYTE );
    LL_DMA_SetMemorySize( DMA1, LL_DMA_CHANNEL_2, LL_DMA_MDATAALIGN_BYTE );
    LL_DMA_SetChannelPriorityLevel( DMA1, LL_DMA_CHANNEL_2,
                                    LL_DMA_PRIORITY_MEDIUM );

    LL_DMA_SetPeriphAddress( DMA1, LL_DMA_CHANNEL_2,
                             (uint32_t)LL_USART_DMA_GetRegAddr( USART2 ) );
    LL_DMA_SetMemoryAddress( DMA1, LL_DMA_CHANNEL_2, (uint32_t)data );
    LL_DMA_SetDataLength( DMA1, LL_DMA_CHANNEL_2, (uint32_t)len );

    LL_USART_EnableDMAReq_TX( USART2 );
    LL_DMA_EnableChannel( DMA1, LL_DMA_CHANNEL_2 );
    return 0u;
}

uint8_t USART2_DMA_TxBusy( void )
{
    return ( LL_DMA_IsEnabledChannel( DMA1, LL_DMA_CHANNEL_2 ) != 0u ) ? 1u : 0u;
}