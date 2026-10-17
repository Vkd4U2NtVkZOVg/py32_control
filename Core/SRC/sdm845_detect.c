/**
 * @file sdm845_detect.c
 * @brief Five-stage SDM845 boot detection from the Linux shell over USART2 DMA.
 *
 * Five independent streaming KMP matchers run side by side over the same byte
 * stream (stage 1 "Boot Config @", stage 2 "login", stage 3 "comma",
 * stage 4 "Welcome to AGNOS", stage 5 "Front Left: Playback"). Each matcher
 * keeps its own fail table and automaton state, so matching survives across
 * poll calls and no target can shadow another. Boot latches only when stage 2
 * has matched AND the line has been idle for SDM845_BOOT_IDLE_MS.
 */
#include "sdm845_detect.h"
#include "usart2_dma.h"
#include "QuarkTS.h"
#include "qport_py32.h"

/*---------------------------------------------------------------------------*/
/* Streaming KMP matcher instance                                             */
/*---------------------------------------------------------------------------*/
typedef struct {
    const char *pattern;                    /* target string                   */
    uint32_t    len;                        /* pattern length                  */
    uint8_t     fail[ SDM845_BANNER_MAX_LEN ];
    uint32_t    q;                          /* matched-prefix length (state)   */
    uint8_t     seen;                       /* latched: matched at least once  */
} Sdm845_Matcher_t;

static Sdm845_Matcher_t s_stage1;           /* "Boot Config @"             */
static Sdm845_Matcher_t s_stage2;           /* "login"                         */
static Sdm845_Matcher_t s_stage3;           /* "Password:" (account accepted)  */
static Sdm845_Matcher_t s_stage4;           /* "Welcome to AGNOS"              */
static Sdm845_Matcher_t s_stage5;           /* "Front Left: Playback"          */

/*---------------------------------------------------------------------------*/
/* Detection results + idle timer                                             */
/*---------------------------------------------------------------------------*/
static uint8_t  s_booted;         /* stage2 seen + idle window elapsed (latch) */
static uint32_t s_last_rx_tick;   /* QOS_Tick_Get() at the last received byte  */
static uint8_t  s_any_rx;         /* any byte ever received                    */

/*---------------------------------------------------------------------------*/
/* Task handle                                                                */
/*---------------------------------------------------------------------------*/
static qTask_t s_task;

/*---------------------------------------------------------------------------*/
/* KMP: build the prefix (failure) function for one matcher                   */
/*---------------------------------------------------------------------------*/
static void sdm845_matcher_init( Sdm845_Matcher_t *m, const char *pattern )
{
    uint32_t len = 0u;
    uint32_t i = 1u;

    m->pattern = pattern;
    m->len     = 0u;
    while ( pattern[ m->len ] != '\0' ) {
        m->len++;
    }

    m->fail[ 0 ] = 0u;
    while ( i < m->len ) {
        if ( pattern[ i ] == pattern[ len ] ) {
            len++;
            m->fail[ i ] = (uint8_t)len;
            i++;
        } else {
            if ( len != 0u ) {
                len = m->fail[ len - 1u ];
            } else {
                m->fail[ i ] = 0u;
                i++;
            }
        }
    }

    m->q    = 0u;
    m->seen = 0u;
}

/* Feed one byte into one matcher; latches m->seen on a full match. */
static void sdm845_matcher_feed( Sdm845_Matcher_t *m, uint8_t c )
{
    while ( ( m->q > 0u ) && ( c != (uint8_t)m->pattern[ m->q ] ) ) {
        m->q = m->fail[ m->q - 1u ];
    }
    if ( c == (uint8_t)m->pattern[ m->q ] ) {
        m->q++;
    }
    if ( m->q == m->len ) {
        m->seen = 1u;
        m->q = m->fail[ m->q - 1u ];   /* allow overlapping later matches */
    }
}

/* Rewind one matcher to its initial automaton state without latching. */
static void sdm845_matcher_reset( Sdm845_Matcher_t *m )
{
    m->q    = 0u;
    m->seen = 0u;
}

/*---------------------------------------------------------------------------*/
/* Periodic task: drain the ring, feed all matchers, track idle.              */
/*---------------------------------------------------------------------------*/
static void sdm845_task_cb( qEvent_t e )
{
    uint8_t  tmp[ 256 ];
    size_t   n;
    size_t   i;

    (void)e;

    n = USART2_DMA_Read( tmp, sizeof( tmp ) );
    if (USART2_DMA_TakeRxLoss()) {
        Sdm845_Detect_ResetAll();
        s_any_rx = 0u;
        return;
    }
    if ( n > 0u ) {
        for ( i = 0u ; i < n ; ++i ) {
            sdm845_matcher_feed( &s_stage1, tmp[ i ] );
            sdm845_matcher_feed( &s_stage2, tmp[ i ] );
            sdm845_matcher_feed( &s_stage3, tmp[ i ] );
            sdm845_matcher_feed( &s_stage4, tmp[ i ] );
            sdm845_matcher_feed( &s_stage5, tmp[ i ] );
        }
        s_last_rx_tick = QOS_Tick_Get();
        s_any_rx       = 1u;
    }

    if ( ( s_stage2.seen != 0u ) &&
         ( Sdm845_Detect_GetIdleMs() >= SDM845_BOOT_IDLE_MS ) ) {
        s_booted = 1u;
    }
}

/*---------------------------------------------------------------------------*/
/* Public API                                                                 */
/*---------------------------------------------------------------------------*/
void Sdm845_Detect_Init( void )
{
    sdm845_matcher_init( &s_stage1, SDM845_BOOT_BANNER );
    sdm845_matcher_init( &s_stage2, SDM845_LOGIN_BANNER );
    sdm845_matcher_init( &s_stage3, SDM845_STAGE3_BANNER );
    sdm845_matcher_init( &s_stage4, SDM845_STAGE4_BANNER );
    sdm845_matcher_init( &s_stage5, SDM845_STAGE5_BANNER );

    s_booted       = 0u;
    s_last_rx_tick = 0u;
    s_any_rx       = 0u;

    (void)qOS_Add_Task( &s_task, sdm845_task_cb, qLowest_Priority,
                        (qTime_t)SDM845_DETECT_PERIOD_S, qPeriodic,
                        qEnabled, NULL );
}

uint8_t Sdm845_Detect_IsStage1Seen( void )
{
    return s_stage1.seen;
}

uint8_t Sdm845_Detect_IsStage2Seen( void )
{
    return s_stage2.seen;
}

uint8_t Sdm845_Detect_IsStage3Seen( void )
{
    return s_stage3.seen;
}

uint8_t Sdm845_Detect_IsStage4Seen( void )
{
    return s_stage4.seen;
}

uint8_t Sdm845_Detect_IsStage5Seen( void )
{
    return s_stage5.seen;
}

void Sdm845_Detect_ResetStage345( void )
{
    sdm845_matcher_reset( &s_stage3 );
    sdm845_matcher_reset( &s_stage4 );
    sdm845_matcher_reset( &s_stage5 );
}

void Sdm845_Detect_ResetAll( void )
{
    sdm845_matcher_reset( &s_stage1 );
    sdm845_matcher_reset( &s_stage2 );
    sdm845_matcher_reset( &s_stage3 );
    sdm845_matcher_reset( &s_stage4 );
    sdm845_matcher_reset( &s_stage5 );
    s_booted = 0u;                       /* IsBooted() must re-latch fresh */
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