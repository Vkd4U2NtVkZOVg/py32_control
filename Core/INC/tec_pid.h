/**
 * @file tec_pid.h
 * @brief TEC PID controller wrapper around the EPID (Embedded-PID) library.
 *
 * This wraps the incremental "Type-C" PID of epid (Drivers/Embedded-PID, kept
 * as a git submodule) into a small TEC-oriented interface: set the target
 * temperature, feed the measured temperature each control tick, and read the
 * controller output (a duty or current command). Anti-windup and output
 * saturation are applied inside.
 *
 * The TEC output is typically bi-directional (cool or heat), so the controller
 * output is produced as a signed value in [-1.0, +1.0] (or any caller-chosen
 * range via TecPid_SetOutputLimits) and the caller maps it to the H-bridge /
 * PWM polarity.
 */
#ifndef __TEC_PID_H__
#define __TEC_PID_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *epid;               /**< opaque epid_t instance (defined in pid.c) */
    float setpoint;           /**< target temperature                          */
    float last_measure;       /**< last injected temperature                   */
    float out_min;            /**< lower output limit                          */
    float out_max;            /**< upper output limit                          */
    float i_min;              /**< anti-windup lower bound on I-term           */
    float i_max;              /**< anti-windup upper bound on I-term           */
    void *lpf;                /**< optional D-term low-pass filter (epid_lpf_t) */
    uint8_t d_filter_on;      /**< !=0 applies the LPF to the D-term            */
} TecPid_t;

/**
 * @brief Initialize the TEC PID controller.
 * @param pid       Controller handle.
 * @param kp,ki,kd  PID gains (Kp must be > 0, Ki > 0, Kd >= 0).
 * @param measure   Initial temperature measurement (for D-term seed).
 * @param out_min   Lower output limit.
 * @param out_max   Upper output limit.
 * @param i_min,i_max Anti-windup I-term limits (e.g. [-out_max, out_max]).
 * @param d_filter_a  D-term LPF smoothing factor (0 = disabled; else 0<a<1).
 * @return 0 on success, or a negative error code.
 */
int TecPid_Init(TecPid_t *pid,
                float kp, float ki, float kd,
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