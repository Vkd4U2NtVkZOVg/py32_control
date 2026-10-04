/**
 * @file usart2_dma.h
 * @brief USART2 (PA0=TX, PA1=RX) DMA receive ring + non-blocking TX using DMA1.
 *
 * RX path: DMA1_Channel1 runs in circular mode and continuously fills a fixed
 * RAM buffer directly from USART2->DR. The SYSCFG_CFGR3 DMA1_MAP field routes
 * the channel to the USART2_RX request (mapping code 0b01000).
 *
 * TX path: DMA1_Channel2 is routed through the independent SYSCFG_CFGR3
 * DMA2_MAP field to the USART2_TX request (mapping code 0b00111) and moved in
 * one non-circular shot per USART2_DMA_Send() call, so the scheduler never
 * blocks on transmission.
 *
 * Unlike a simple IDLE-interrupt scheme, this module does NOT decide frame
 * boundaries. It just exposes the newest received bytes via USART2_DMA_Read().
 * Frame/payload decision (content matching, idle timeout, boot detection) is
 * done by a higher layer (see sdm845_detect.c), which reads the ring whenever
 * it runs. This avoids the IDLE-line pitfall where a Linux shell's continuous
 * \r\n output never lets the line go idle.
 *
 * Reference: PY32F030 Reference Manual V1.6, SYSCFG_CFGR3 (10.1.3),
 * DMA request mapping (Table 11-3), USART_CR3.DMAR/DMAT (26.5.6).
 */
#ifndef __USART2_DMA_H__
#define __USART2_DMA_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Size of the circular DMA receive buffer. Must be <= 0xFFFF (DMA_NbData). */
#define USART2_DMA_RX_BUF_LEN   (256u)

/**
 * @brief Configure USART2 DMA reception and start receiving.
 *
 * Prerequisites: USART2 must already be initialized (Studio_USART2_Init) so
 * PA0/PA1 are muxed and the UART clock/enable is done. This function routes
 * DMA1_Channel1 to USART2_RX and starts the circular transfer.
 */
void USART2_DMA_Start(void);

/**
 * @brief Stop DMA reception (disables the DMA channel and the USART DMAR
 *        request). Data already in the buffer is preserved.
 */
void USART2_DMA_Stop(void);

/**
 * @brief Copy the newest received bytes since the last call out of the ring
 *        buffer. Bytes are returned oldest-first.
 * @param out       Destination buffer (at least `max_len` bytes).
 * @param max_len   Maximum number of bytes to copy.
 * @return Number of bytes actually copied (0 if nothing new since last call).
 */
size_t USART2_DMA_Read(uint8_t *out, size_t max_len);

/**
 * @brief Queue one non-blocking transmission on DMA1_Channel2 (USART2_TX).
 *
 * Starts a mem->periph, non-circular DMA move of `len` bytes from `data` and
 * returns immediately: the caller polls USART2_DMA_TxBusy() (or simply waits
 * for the next task run) instead of spinning. `data` must stay valid until
 * the transfer completes, so pass a static/string-literal buffer.
 *
 * Prerequisites: the SoC RX line must already be routed to the PY32 TX, i.e.
 * call USART2_SetTxSource(USART2_TXSRC_PY32) first when the PF3 SPDT is on
 * the CH343 throw.
 *
 * @param data  Bytes to send (literal/static storage; not copied).
 * @param len   Number of bytes (clamped to 0xFFFF, the DMA_NbData limit).
 * @return 0 on success, 1 if a transfer is still in progress (call later).
 */
uint8_t USART2_DMA_Send(const uint8_t *data, size_t len);

/**
 * @brief Non-zero while the TX DMA transfer started by USART2_DMA_Send() is
 *        still running. Concurrent sends are rejected (returns busy) instead
 *        of queued: at most one transfer is ever in flight.
 */
uint8_t USART2_DMA_TxBusy(void);

#ifdef __cplusplus
}
#endif

#endif /* __USART2_DMA_H__ */