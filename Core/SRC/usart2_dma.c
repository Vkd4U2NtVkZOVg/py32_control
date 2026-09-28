/**
 * @file usart2_dma.c
 * @brief USART2 (PA0/PA1) DMA receive ring buffer using DMA1_Channel1.
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

    /* 1) Route DMA1_Channel1 to the USART2_RX request via SYSCFG_CFGR3. */
    MODIFY_REG( SYSCFG->CFGR3, SYSCFG_CFGR3_DMA1_MAP_Msk,
                (0x08UL << SYSCFG_CFGR3_DMA1_MAP_Pos) );   /* 01000 = USART2_RX */

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