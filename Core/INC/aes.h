/**
 * @file aes.h
 * @brief Software AES-128 for the TRES challenge-response authentication.
 *
 * Compile-time optional via TRES_AES_ENABLE (see tres_i2c_slave.h). This
 * module provides a compact table-driven AES-128 ECB encryptor and the
 * device key. It runs entirely on the MCU (no third-party crypto stack):
 * ~2.5 KB flash, ~176 B stack for the expanded key, one 16-byte block in
 * well under 1 ms at 32 MHz Cortex-M0+.
 *
 * SECURITY NOTE: the device key lives in FLASH as a plain constant. This
 * protects against casual cloning (the host must know the key to verify),
 * not against an attacker who can read the firmware (JTAG/SWD, flash read).
 * It is "防君子不防小人" - if the threat model needs a physically
 * unreadable key, a real ATECC608-style secure element is required instead.
 */
#ifndef __AES_H__
#define __AES_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================*/
/* Compile-time switch                                                         */
/*                                                                            */
/* TRES_AES_ENABLE is defined in the build (eide.yml -D) to pull the AES       */
/* auth feature in. When undefined, aes.c is not compiled, the I2C auth        */
/* windows do not exist and nothing here is referenced.                        */
/*                                                                            */
/* It is normally defined from the build settings; keep this fallback so a     */
/* bare "gcc aes.c" compile (and the header's self-consistency) still works.  */
/*============================================================================*/
#ifndef TRES_AES_ENABLE
#define TRES_AES_ENABLE 0
#endif

#if ( TRES_AES_ENABLE == 1 )

/*============================================================================*/
/* Device key (16 bytes = AES-128)                                            */
/*                                                                            */
/* Same value must be known by the verifying host (tres_i2c_auth.py). Change  */
/* per product line to invalidate old boards. Keep it out of any place a      */
/* DEFAULT_PARAMS / NVRAM write could alter: this is a compile-time constant, */
/* not an NVRAM slot.                                                         */
/*============================================================================*/
#define AES_DEVICE_KEY_BYTES \
    { 0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6, \
      0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C }

/**
 * @brief Encrypt one 16-byte block (AES-128, ECB, no padding).
 * @param key    16-byte device key (read only).
 * @param in     16-byte plaintext block.
 * @param out    16-byte ciphertext block (may alias @p in).
 */
void Aes128_Encrypt( const uint8_t key[ 16 ],
                     const uint8_t in[ 16 ],
                     uint8_t out[ 16 ] );

#endif /* TRES_AES_ENABLE == 1 */

#ifdef __cplusplus
}
#endif

#endif /* __AES_H__ */