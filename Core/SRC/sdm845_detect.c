/**
 * @file sdm845_detect.c
 * @brief SDM845 boot-success detection from the Linux shell over USART2 DMA.
 *
 * Uses a streaming KMP matcher for the boot banner so matching state survives
 * across poll calls (robust to continuous shell output), plus an idle timer
 * driven by QOS_Tick_Get() (1 ms time base). Boot is declared only when the
 * banner has matched AND the line has been quiet for SDM845_BOOT_IDLE_MS.
 */
#include "sdm845_detect.h"
#include "usart2_dma.h"
#include "QuarkTS.h"
#include "qport_py32.h"

/*---------------------------------------------------------------------------*/
/* Streaming KMP matcher state                                                */
/*---------------------------------------------------------------------------*/
static const char s_pattern[ SDM845_BOOT_BANNER_LEN ] = SDM845_BOOT_BANNER;
static uint8_t  s_fail[ SDM845_BOOT_BANNER_LEN ];
static uint32_t s_q;              /* KMP match state: how much of banner matched */

/*---------------------------------------------------------------------------*/
/* Detection results + idle timer                                             */
/*---------------------------------------------------------------------------*/
static uint8_t  s_banner_seen;    /* banner matched at least once              */
static uint8_t  s_booted;         /* banner seen + idle window elapsed (latch) */
static uint32_t s_last_rx_tick;   /* QOS_Tick_Get() at the last received byte  */
static uint8_t  s_any_rx;         /* any byte ever received                    */

/*---------------------------------------------------------------------------*/
/* Task handle                                                                */
/*---------------------------------------------------------------------------*/
static qTask_t s_task;

/*---------------------------------------------------------------------------*/
/* KMP prefix (failure) function                                              */
/*---------------------------------------------------------------------------*/
static void sdm845_kmp_build( void )
{
    uint32_t len = 0u;
    uint32_t i = 1u;

    s_fail[ 0 ] = 0u;
    while ( i < SDM845_BOOT_BANNER_LEN ) {
        if ( s_pattern[ i ] == s_pattern[ len ] ) {
            len++;
            s_fail[ i ] = (uint8_t)len;
            i++;
        } else {
            if ( len != 0u ) {
                len = s_fail[ len - 1u ];
            } else {
                s_fail[ i ] = 0u;
                i++;
            }
        }
    }
}

/* Feed one byte; sets s_banner_seen when the banner is matched. */
static void sdm845_feed( uint8_t c )
{
    const uint8_t b = (uint8_t)c;

    while ( s_q > 0u && b != (uint8_t)s_pattern[ s_q ] ) {
        s_q = s_fail[ s_q - 1u ];
    }
    if ( b == (uint8_t)s_pattern[ s_q ] ) {
        s_q++;
    }
    if ( s_q == SDM845_BOOT_BANNER_LEN ) {
        s_banner_seen = 1u;
        s_q = s_fail[ s_q - 1u ];   /* allow overlapping later matches */
    }
}

/*---------------------------------------------------------------------------*/
/* Periodic task: drain the ring, feed the matcher, track idle.               */
/*---------------------------------------------------------------------------*/
static void sdm845_task_cb( qEvent_t e )
{
    uint8_t  tmp[ 64 ];
    size_t   n;
    size_t   i;

    (void)e;

    n = USART2_DMA_Read( tmp, sizeof( tmp ) );
    if ( n > 0u ) {
        for ( i = 0u ; i < n ; ++i ) {
            sdm845_feed( tmp[ i ] );
        }
        s_last_rx_tick = QOS_Tick_Get();
        s_any_rx       = 1u;
    }

    if ( ( s_banner_seen != 0u ) &&
         ( Sdm845_Detect_GetIdleMs() >= SDM845_BOOT_IDLE_MS ) ) {
        s_booted = 1u;
    }
}

/*---------------------------------------------------------------------------*/
/* Public API                                                                 */
/*---------------------------------------------------------------------------*/
void Sdm845_Detect_Init( void )
{
    sdm845_kmp_build();
    s_q            = 0u;
    s_banner_seen  = 0u;
    s_booted       = 0u;
    s_last_rx_tick = 0u;
    s_any_rx       = 0u;

    (void)qOS_Add_Task( &s_task, sdm845_task_cb, qLowest_Priority,
                        (qTime_t)SDM845_DETECT_PERIOD_S, qPeriodic,
                        qEnabled, NULL );
}

uint8_t Sdm845_Detect_IsBooted( void )
{
    return s_booted;
}

uint32_t Sdm845_Detect_GetIdleMs( void )
{
    if ( s_any_rx == 0u ) {
        return 0u;
    }
    return (uint32_t)( QOS_Tick_Get() - s_last_rx_tick );
}