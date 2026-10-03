/**
 * @file sdm845_detect.h
 * @brief Five-stage SDM845 boot detection from the Linux shell over USART2 DMA.
 *
 * This layer reads the USART2 DMA ring buffer (usart2_dma.c) and tracks the
 * SoC boot progress through FIVE independent streaming KMP matchers:
 *
 *   stage 1: "Loader Build Info"    - the bootloader banner (SoC is alive)
 *   stage 2: "login"                - a getty prompt (Linux user space is up)
 *   stage 3: "comma"                - echo of the account typed at the prompt
 *   stage 4: "Welcome to AGNOS"     - post-login banner (authentication passed)
 *   stage 5: "Front Left: Playback" - amixer reply (the SoC accepts commands)
 *
 * Matching state survives across poll calls (robust to a continuous stream of
 * \r\n shell output), and a stage only latches when its exact string appears
 * as a contiguous byte run. Stage 3 relies on the getty/tty echoing the typed
 * account, not on any shell prompt character.
 *
 * "Boot complete" means stage 2 has been seen AND the line has been quiet for
 * >= SDM845_BOOT_IDLE_MS; Sdm845_Detect_IsBooted() reports exactly that (kept
 * for the retry-decision path). The staged latches (IsStage1Seen..IsStage5Seen)
 * let the boot state machine advance through STAGE3/4/5_WAIT. ResetStage345()
 * clears the stage 3/4/5 latches and automaton states so a retry pass starts
 * clean (stage 1/2 latches are never cleared: they are per-power-cycle facts).
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

/** Stage 3 target: echoed account sent at the login prompt. */
#define SDM845_STAGE3_BANNER    "comma"

/** Stage 4 target: post-login AGNOS banner (password accepted). */
#define SDM845_STAGE4_BANNER    "Welcome to AGNOS"

/** Stage 5 target: amixer reply line (SoC shell accepts commands). */
#define SDM845_STAGE5_BANNER    "Front Left: Playback"

/* String lengths remain derived from the string literals, while enum values
 * are valid integer constant expressions usable for array sizes and switch/
 * preprocessor-independent compile-time layout. */
enum {
    SDM845_BOOT_BANNER_LEN  = (int)(sizeof(SDM845_BOOT_BANNER) - 1u),
    SDM845_LOGIN_BANNER_LEN = (int)(sizeof(SDM845_LOGIN_BANNER) - 1u),
    SDM845_STAGE3_BANNER_LEN = (int)(sizeof(SDM845_STAGE3_BANNER) - 1u),
    SDM845_STAGE4_BANNER_LEN = (int)(sizeof(SDM845_STAGE4_BANNER) - 1u),
    SDM845_STAGE5_BANNER_LEN = (int)(sizeof(SDM845_STAGE5_BANNER) - 1u),
    SDM845_BANNER_MAX_LEN   = ( (SDM845_BOOT_BANNER_LEN  > SDM845_LOGIN_BANNER_LEN) &&
                                (SDM845_BOOT_BANNER_LEN  > SDM845_STAGE3_BANNER_LEN) &&
                                (SDM845_BOOT_BANNER_LEN  > SDM845_STAGE4_BANNER_LEN) &&
                                (SDM845_BOOT_BANNER_LEN  > SDM845_STAGE5_BANNER_LEN) )
                              ? SDM845_BOOT_BANNER_LEN
                              : ( (SDM845_STAGE4_BANNER_LEN > SDM845_STAGE5_BANNER_LEN)
                                  ? SDM845_STAGE4_BANNER_LEN
                                  : SDM845_STAGE5_BANNER_LEN )
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
 * @brief Stage 3 latch: echoed account "comma" seen (getty accepted input).
 */
uint8_t Sdm845_Detect_IsStage3Seen(void);

/**
 * @brief Stage 4 latch: "Welcome to AGNOS" seen (password accepted).
 */
uint8_t Sdm845_Detect_IsStage4Seen(void);

/**
 * @brief Stage 5 latch: "Front Left: Playback" seen (command accepted).
 */
uint8_t Sdm845_Detect_IsStage5Seen(void);

/**
 * @brief Clear the stage 3/4/5 latches and rewind their KMP automaton states.
 *        Call on entering STAGE3_WAIT so each retry pass re-matches its own
 *        echoes; stage 1/2 latches are deliberately left untouched.
 */
void Sdm845_Detect_ResetStage345(void);

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