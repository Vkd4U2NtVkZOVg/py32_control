/**
 * @file sdm845_detect.h
 * @brief SDM845 boot-success detection from the Linux shell over USART2 DMA.
 *
 * This layer reads the USART2 DMA ring buffer (usart2_dma.c) and decides
 * whether the target SoC has brought up Linux by matching a boot banner string
 * and measuring how long the shell has been quiet. It is built to be robust to
 * a continuous stream of \r\n output from the shell: the boot string is matched
 * with a streaming KMP automaton (state survives across poll calls), and "boot
 * complete" additionally requires the line to have been idle for a while, so
 * transient banner fragments cannot cause a false positive.
 *
 * Boot is considered successful when BOTH conditions hold:
 *   - the target string "Loader Build Info" has been seen in the RX stream, and
 *   - the USART line has been quiet (no new byte) for >= SDM845_BOOT_IDLE_MS.
 *
 * A periodic QuarkTS task polls the ring and runs the matcher. The SDM845 boot
 * state machine queries Sdm845_Detect_IsBooted() when it needs to confirm the
 * boot chain and Sdm845_Detect_GetIdleMs() to drive its own idle timeout.
 */
#ifndef __SDM845_DETECT_H__
#define __SDM845_DETECT_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Target banner string that marks a successful boot. */
#define SDM845_BOOT_BANNER      "Loader Build Info"
#define SDM845_BOOT_BANNER_LEN  (sizeof(SDM845_BOOT_BANNER) - 1u)

/** Idle time (ms) that must elapse after the banner to declare "boot OK". */
#define SDM845_BOOT_IDLE_MS     (2000u)

/** Poll period (s) of the detection task (also the RX ring read period). */
#define SDM845_DETECT_PERIOD_S  (0.02f)   /* 20 ms */

/**
 * @brief Register and start the SDM845 boot-detection task. Reads the USART2
 *        DMA ring, feeds the banner KMP matcher, and tracks the idle timer.
 *        Call once after qOS_Setup and USART2_DMA_Start().
 */
void Sdm845_Detect_Init(void);

/**
 * @brief Non-zero once the boot banner has been observed and the line has been
 *        idle for the required window. Latches true (never clears).
 */
uint8_t Sdm845_Detect_IsBooted(void);

/**
 * @brief Milliseconds since the last received byte on USART2 (idle duration).
 *        Updated by the detection task; 0 if no data has ever arrived.
 */
uint32_t Sdm845_Detect_GetIdleMs(void);

#ifdef __cplusplus
}
#endif

#endif /* __SDM845_DETECT_H__ */