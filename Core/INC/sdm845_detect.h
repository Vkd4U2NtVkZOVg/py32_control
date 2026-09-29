/**
 * @file sdm845_detect.h
 * @brief Two-stage SDM845 boot detection from the Linux shell over USART2 DMA.
 *
 * This layer reads the USART2 DMA ring buffer (usart2_dma.c) and tracks the
 * SoC boot progress through TWO independent streaming KMP matchers:
 *
 *   stage 1: "Loader Build Info"  - the bootloader banner (SoC is alive)
 *   stage 2: "login"              - a getty prompt (Linux user space is up)
 *
 * Matching state survives across poll calls (robust to a continuous stream of
 * \r\n shell output), and a stage only latches when its exact string appears
 * as a contiguous byte run.
 *
 * "Boot complete" means stage 2 has been seen AND the line has been quiet for
 * >= SDM845_BOOT_IDLE_MS; Sdm845_Detect_IsBooted() reports exactly that. The
 * individual stage latches (IsStage1Seen / IsStage2Seen) let the boot state
 * machine fail with better precision: a loader without a login points at a
 * kernel/init hang, not at a dead SoC.
 */
#ifndef __SDM845_DETECT_H__
#define __SDM845_DETECT_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Stage 1 target: bootloader banner (SoC bootloader is alive). */
#define SDM845_BOOT_BANNER      "Loader Build Info"

/** Stage 2 target: getty login prompt (Linux user space is up). */
#define SDM845_LOGIN_BANNER     "login"

/* String lengths remain derived from the string literals, while enum values
 * are valid integer constant expressions usable for array sizes and switch/
 * preprocessor-independent compile-time layout. */
enum {
    SDM845_BOOT_BANNER_LEN  = (int)(sizeof(SDM845_BOOT_BANNER) - 1u),
    SDM845_LOGIN_BANNER_LEN = (int)(sizeof(SDM845_LOGIN_BANNER) - 1u),
    SDM845_BANNER_MAX_LEN   = (SDM845_BOOT_BANNER_LEN > SDM845_LOGIN_BANNER_LEN)
                              ? SDM845_BOOT_BANNER_LEN : SDM845_LOGIN_BANNER_LEN
};

/** Idle time (ms) that must elapse after stage 2 to declare "boot OK". */
#define SDM845_BOOT_IDLE_MS     (2000u)

/** Poll period (s) of the detection task (also the RX ring read period). */
#define SDM845_DETECT_PERIOD_S  (0.02f)   /* 20 ms */

/**
 * @brief Register and start the SDM845 boot-detection task. Reads the USART2
 *        DMA ring, feeds both stage matchers, and tracks the idle timer.
 *        Call once after qOS_Setup and USART2_DMA_Start().
 */
void Sdm845_Detect_Init(void);

/**
 * @brief Stage 1 latch: "Loader Build Info" has been seen in the RX stream.
 */
uint8_t Sdm845_Detect_IsStage1Seen(void);

/**
 * @brief Stage 2 latch: "login" has been seen in the RX stream.
 */
uint8_t Sdm845_Detect_IsStage2Seen(void);

/**
 * @brief Non-zero once stage 2 ("login") has been observed AND the line has
 *        been idle for the required window. Latches true (never clears).
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