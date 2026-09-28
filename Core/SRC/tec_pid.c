/**
 * @file tec_pid.c
 * @brief TEC PID controller wrapper around the EPID library.
 */
#include "tec_pid.h"
#include "pid.h"   /* EPID library */

#include <stddef.h>   /* NULL */

#define TEC_PID_OK           (0)
#define TEC_PID_ERR_INIT     (-1)
#define TEC_PID_ERR_PARAM    (-2)

int TecPid_Init(TecPid_t *pid,
                float kp, float ki, float kd,
                float measure,
                float out_min, float out_max,
                float i_min, float i_max,
                float d_filter_a)
{
    epid_t *ctx;
    epid_info_t info;

    if ( pid == NULL ) {
        return TEC_PID_ERR_PARAM;
    }

    /* Allocate the epid and optional D-filter instances up front. */
    static epid_t s_epid;
    static epid_lpf_t s_lpf;
    ctx = &s_epid;

    info = epid_init(ctx, measure, measure, 0.0f, kp, ki, kd);
    if ( info != EPID_ERR_NONE ) {
        return TEC_PID_ERR_INIT;
    }

    pid->epid          = ctx;
    pid->setpoint      = measure;   /* default SP == initial measure */
    pid->last_measure  = measure;
    pid->out_min       = out_min;
    pid->out_max       = out_max;
    pid->i_min         = i_min;
    pid->i_max         = i_max;
    pid->d_filter_on   = ( d_filter_a > 0.0f ) ? 1u : 0u;
    pid->lpf           = &s_lpf;

    if ( pid->d_filter_on != 0u ) {
        if ( epid_util_lpf_init(&s_lpf, d_filter_a, measure) != EPID_ERR_NONE ) {
            return TEC_PID_ERR_INIT;
        }
    } else {
        pid->lpf = NULL;
    }

    return TEC_PID_OK;
}

void TecPid_SetSetpoint(TecPid_t *pid, float setpoint)
{
    if ( pid != NULL ) {
        pid->setpoint = setpoint;
    }
}

void TecPid_Update(TecPid_t *pid, float measure)
{
    epid_t *ctx;
    float d_term;

    if ( pid == NULL || pid->epid == NULL ) {
        return;
    }
    ctx = (epid_t *)pid->epid;

    /* Compute P/I/D terms (incremental Type-C PID). */
    epid_pid_calc(ctx, pid->setpoint, measure);

    /* Anti-windup: clamp the I-term before summing. */
    epid_util_ilim(ctx, pid->i_min, pid->i_max);

    /* Optional D-term low-pass filtering (noise on temperature sensing). */
    if ( pid->d_filter_on != 0u && pid->lpf != NULL ) {
        d_term = ((epid_t *)pid->epid)->d_term;
        epid_util_lpf_calc((epid_lpf_t *)pid->lpf, d_term);
        ((epid_t *)pid->epid)->d_term = ((epid_lpf_t *)pid->lpf)->y;
    }

    /* Accumulate and saturate the output. */
    epid_pid_sum(ctx, pid->out_min, pid->out_max);

    pid->last_measure = measure;
}

float TecPid_GetOutput(const TecPid_t *pid)
{
    if ( pid == NULL || pid->epid == NULL ) {
        return 0.0f;
    }
    return ((epid_t *)pid->epid)->y_out;
}

void TecPid_Reset(TecPid_t *pid, float measure)
{
    epid_t *ctx;

    if ( pid == NULL || pid->epid == NULL ) {
        return;
    }
    ctx = (epid_t *)pid->epid;

    epid_init(ctx, measure, measure, 0.0f,
              ctx->kp, ctx->ki, ctx->kd);
    pid->setpoint     = measure;
    pid->last_measure = measure;
}