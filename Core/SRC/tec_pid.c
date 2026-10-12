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
                float period_s,
                float measure,
                float out_min, float out_max,
                float i_min, float i_max,
                float d_filter_a)
{
    epid_t *ctx;
    epid_info_t info;
    float kp_eff, ki_eff, kd_eff;

    if ( pid == NULL ) {
        return TEC_PID_ERR_PARAM;
    }

    /* Sampling-period normalization: the EPID is incremental (velocity form),
     * so the per-cycle equivalents of the physical (per-second) gains are:
     *   I: position form integrates e*Ts -> per-cycle increment is Ki_c*Ts
     *      ki_eff = ki * period_s                     (ki input: %/(mdegC*s))
     *   D: position form differentiates x -> its one-cycle INCREMENT is
     *      (Kd_c/Ts)*(2x[k-1]-x[k-2]-x[k])            (second difference)
     *      kd_eff = kd / period_s                     (kd input: %/(mdegC*s))
     * The 1/Ts for D matches the submodule's own epid_init_T
     * (kd = kp*Td/sample_period). The old kd*Ts here was inverted - it made
     * the D term off by 1/Ts^2 and was masked at the original Ts = 1 s. */
    if ( period_s <= 0.0f ) {
        period_s = 1.0f;
    }
    kp_eff = kp;
    ki_eff = ki * period_s;
    kd_eff = kd / period_s;

    /* The epid and LPF instances live INSIDE the caller's TecPid_t (embedded
     * union storage), so every PID handle owns its own state - no shared
     * singleton, safe for multiple instances. */
    ctx = &pid->epid.epid;

    info = epid_init(ctx, measure, measure, 0.0f, kp_eff, ki_eff, kd_eff);
    if ( info != EPID_ERR_NONE ) {
        return TEC_PID_ERR_INIT;
    }

    pid->setpoint      = measure;   /* default SP == initial measure */
    pid->last_measure  = measure;
    pid->out_min       = out_min;
    pid->out_max       = out_max;
    pid->i_min         = i_min;
    pid->i_max         = i_max;
    /* D-term filter smoothing is per-cycle; the caller derives it from a
     * PHYSICAL filter time constant and the actual period
     * (a = Ts/(tau+Ts), see TEC_D_FILTER_TAU_S in tec_control.c), so the
     * filter dynamics stay period-independent. */
    pid->d_filter_on   = ( d_filter_a > 0.0f ) ? 1u : 0u;

    if ( pid->d_filter_on != 0u ) {
        /* D-term LPF must start from 0 (the D-term is 0 at init because
         * xk_1 == xk_2 == measure): seeding it with `measure` (e.g. +45 V
         * milli-degC) would make the first filtered D output ~a*measure and
         * slam the controller to max output even at zero error, decaying
         * only over ~1/a cycles. */
        float lpf_seed = 0.0f;
        if ( epid_util_lpf_init( &pid->lpf.lpf,
                                 d_filter_a, lpf_seed ) != EPID_ERR_NONE ) {
            return TEC_PID_ERR_INIT;
        }
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
    float error;

    if ( pid == NULL ) {
        return;
    }
    ctx = &pid->epid.epid;

    /* Compute P/I/D terms (incremental Type-C PID). */
    epid_pid_calc(ctx, pid->setpoint, measure);
    error = pid->setpoint - measure;

    /* Anti-windup: clamp the I-term before summing. */
    epid_util_ilim(ctx, pid->i_min, pid->i_max);

    /* Optional D-term low-pass filtering (noise on temperature sensing). */
    if ( pid->d_filter_on != 0u ) {
        d_term = ctx->d_term;
        epid_util_lpf_calc( &pid->lpf.lpf, d_term );
        ctx->d_term = pid->lpf.lpf.y;
    }

    /* Conditional integration (anti-windup #2): if the output is already
     * pinned at a limit and the current error would push it further into
     * that same limit, drop the I-term for this cycle. Without this, the
     * integrator keeps winding up while the output is saturated and the loop
     * overshoots badly when the disturbance clears. */
    if ( ( ( ctx->y_out >= pid->out_max ) && ( error > 0.0f ) ) ||
         ( ( ctx->y_out <= pid->out_min ) && ( error < 0.0f ) ) ) {
        ctx->i_term = 0.0f;
    }

    /* Accumulate and saturate the output. */
    epid_pid_sum(ctx, pid->out_min, pid->out_max);

    pid->last_measure = measure;
}

float TecPid_GetOutput(const TecPid_t *pid)
{
    if ( pid == NULL ) {
        return 0.0f;
    }
    return pid->epid.epid.y_out;
}

void TecPid_Reset(TecPid_t *pid, float measure)
{
    epid_t *ctx;

    if ( pid == NULL ) {
        return;
    }
    ctx = &pid->epid.epid;

    epid_init(ctx, measure, measure, 0.0f,
              ctx->kp, ctx->ki, ctx->kd);
    pid->setpoint     = measure;
    pid->last_measure = measure;

    /* D-term filter state must be cleared too, otherwise a stale LPF output
     * (possibly large, from before the fault) leaks into the output for ~1/a
     * cycles after recovery. Seed from 0: the D-term is 0 right after reset
     * (xk_1 == xk_2 == measure). */
    if ( pid->d_filter_on != 0u ) {
        (void)epid_util_lpf_init( &pid->lpf.lpf, pid->lpf.lpf.smoothing_factor, 0.0f );
    }
}
void TecPid_TrackOutput(TecPid_t *pid, float measure, float output)
{
    if (pid == NULL) return;
    TecPid_Reset(pid, measure);
    if (output < pid->out_min) output = pid->out_min;
    if (output > pid->out_max) output = pid->out_max;
    pid->epid.epid.y_out = output;
}
