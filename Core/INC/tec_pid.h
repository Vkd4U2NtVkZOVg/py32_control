/**
 * @file tec_pid.h
 * @brief TEC PID controller wrapper around the EPID (Embedded-PID) library.
 *
 * This wraps the incremental "Type-C" PID of epid (Drivers/Embedded-PID, kept
 * as a git submodule) into a small TEC-oriented interface: set the target
 * temperature, feed the measured temperature each control tick, and read the
 * controller output (a duty command). Anti-windup and output saturation are
 * applied inside.
 *
 * UNIT / TIMING CONVENTION (kept consistent across the whole control chain):
 *   - input  : temperature in milli-degC (setpoint and measure)
 *   - output : duty in percent, [out_min, out_max] = [0, 100] for this
 *              unidirectional cooling application
 *   - kp     : P gain, percent output per milli-degC of error
 *   - ki     : I gain, percent output per milli-degC per SECOND; the wrapper
 *              scales it by the actual loop period internally (ki_eff=ki*Ts)
 *   - kd     : D gain, percent output per milli-degC per SECOND; the wrapper
 *              DIVIDES by the loop period (kd_eff=kd/Ts) - the incremental
 *              second-difference term carries a 1/Ts factor (matches
 *              epid_init_T's kp*Td/Ts)
 *   - period_s: the control-loop period in seconds, passed at init and used
 *              both to normalize ki/kd and to schedule the QuarkTS task, so
 *              tuned gains stay valid at any loop rate.
 * The EPID is "heating semantics"; the caller (tec_control.c) flips the sign
 * of setpoint and measure to obtain cooling error, and drives the actuator
 * through an inverted FB-injection mapping (high DAC = low TEC voltage).
 */
#ifndef __TEC_PID_H__
#define __TEC_PID_H__

#include <stdint.h>

/* Bring in the real epid_t / epid_lpf_t definitions so the embedded storage
 * unions below are complete types (pid.h has its own include guard). */
#include "pid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Storage layout must match the real structs in pid.h exactly:
 *   epid_t   = 10 floats (kp,ki,kd, xk_1,xk_2, p,i,d, y_out)
 *   epid_lpf_t = 2 floats (smoothing_factor, y)
 * Using a union instead of a reinterpret-cast keeps strict-aliasing clean
 * (the active member is set by epid_init / epid_util_lpf_init through the
 * union's epid member). */
typedef union {
    epid_t       epid;        /**< the real Embedded-PID context             */
    float        raw[ 10 ];   /**< same bytes, so TecPid_t is trivially sized */
} TecPid_Epid_t;

typedef union {
    epid_lpf_t   lpf;         /**< the real D-term low-pass filter           */
    float        raw[ 2 ];    /**< same bytes                                 */
} TecPid_Lpf_t;

typedef struct {
    TecPid_Epid_t  epid;      /**< embedded, per-instance PID state          */
    TecPid_Lpf_t   lpf;       /**< embedded, per-instance D-term LPF         */
    float setpoint;           /**< target temperature (milli-degC)           */
    float last_measure;       /**< last injected temperature (milli-degC)    */
    float out_min;            /**< lower output limit (percent)              */
    float out_max;            /**< upper output limit (percent)              */
    float i_min;              /**< anti-windup lower bound on I-increment    */
    float i_max;              /**< anti-windup upper bound on I-increment    */
    uint8_t d_filter_on;      /**< !=0 applies the LPF to the D-term         */
} TecPid_t;

/**
 * @brief Initialize the TEC PID controller.
 *
 * Units and timing convention (all consistent, see note above):
 *   - measure / setpoint : milli-degC
 *   - output             : signed duty in [out_min, out_max] (0..100)
 *   - kp                 : P gain, output per milli-degC of error
 *   - ki                 : I gain, output per milli-degC per SECOND
 *                         (internally scaled by period_s: ki_eff = ki*Ts)
 *   - kd                 : D gain, output per milli-degC per SECOND
 *                         (internally scaled by period_s: kd_eff = kd/Ts)
 *   - period_s           : actual control-loop period, seconds. Tuned gains
 *                          stay valid at any period; only this parameter
 *                          changes when the task period changes.
 *   - i_min/i_max        : anti-windup window for the raw I increment
 *                          (per-cycle; scale a physical %/s slew by Ts), e.g.
 *                          [-out_max, out_max].
 * @param pid       Controller handle.
 * @param kp,ki,kd  PID gains (Kp must be > 0, Ki > 0, Kd >= 0).
 * @param period_s  Control-loop period in seconds (scales ki/kd to per cycle).
 * @param measure   Initial temperature measurement (for D-term seed).
 * @param out_min   Lower output limit.
 * @param out_max   Upper output limit.
 * @param i_min,i_max Anti-windup I-increment limits.
 * @param d_filter_a  D-term LPF smoothing factor (0 = disabled; else 0<a<1).
 * @return 0 on success, or a negative error code.
 */
int TecPid_Init(TecPid_t *pid,
                float kp, float ki, float kd,
                float period_s,
                float measure,
                float out_min, float out_max,
                float i_min, float i_max,
                float d_filter_a);

/**
 * @brief Set the target temperature (SP).
 */
void TecPid_SetSetpoint(TecPid_t *pid, float setpoint);

/**
 * @brief Inject the measured temperature and update the controller.
 *        Call once per control loop tick.
 */
void TecPid_Update(TecPid_t *pid, float measure);

/**
 * @brief Get the last computed controller output (already saturated to
 *        [out_min, out_max]).
 */
float TecPid_GetOutput(const TecPid_t *pid);

/**
 * @brief Reset the controller state (integrator and history) to the given
 *        measurement, keeping gains and limits.
 */
void TecPid_Reset(TecPid_t *pid, float measure);

#ifdef __cplusplus
}
#endif

#endif /* __TEC_PID_H__ */