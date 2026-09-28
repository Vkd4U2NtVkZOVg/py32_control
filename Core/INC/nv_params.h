/**
 * @file nv_params.h
 * @brief Non-volatile parameter storage over the internal FLASH.
 *
 * A fixed-size table of uint32 parameters is kept in a RAM cache and
 * persisted to the last FLASH sector on demand. The on-flash block is a
 * single 128-byte page: magic + version + crc32 + 29 parameter slots.
 *
 *   Base: 0x0800F000 (last sector of the 64 KB device, page-aligned).
 *   Layout: [ magic ][ version ][ crc32 ][ params[0] .. params[28] ]
 *
 * Write strategy is single-block rewrite: NvParams_Save() erases the one page
 * and programms the whole block again. Endurance is ~10k erase cycles on PY32,
 * which is fine for parameters that are written rarely (calibration, config).
 *
 * Usage:
 *   NvParams_Init();                 // once, at power-up (loads or defaults)
 *   ... NvParams_Get(id) / NvParams_Set(id, v) ...
 *   NvParams_Save();                 // persist the RAM cache to FLASH
 */
#ifndef __NV_PARAMS_H__
#define __NV_PARAMS_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many uint32 parameter slots are persisted. The on-flash block is a
 * fixed 128-byte page: magic(4) + version(4) + crc32(4) + 29 * 4 = 128. */
#define NVM_PARAM_COUNT      (29u)

/**
 * @brief Parameter identifiers (slots). Add application-specific slots here,
 *        but do not change the numeric order of existing ids once deployed.
 */
typedef enum {
    NV_PARAM_0 = 0u,          /**< user parameter slot 0                          */
    NV_PARAM_1,               /**< user parameter slot 1                          */
    NV_PARAM_2,               /**< user parameter slot 2                          */
    NV_PARAM_3,               /**< user parameter slot 3                          */
    NV_PARAM_4,               /**< user parameter slot 4                          */
    NV_PARAM_5,               /**< user parameter slot 5                          */
    NV_PARAM_6,               /**< user parameter slot 6                          */
    NV_PARAM_7,               /**< user parameter slot 7                          */
    NV_PARAM_8,               /**< user parameter slot 8                          */
    NV_PARAM_9,               /**< user parameter slot 9                          */
    NV_PARAM_10,              /**< user parameter slot 10                         */
    NV_PARAM_11,              /**< user parameter slot 11                         */
    NV_PARAM_12,              /**< user parameter slot 12                         */
    NV_PARAM_13,              /**< user parameter slot 13                         */
    NV_PARAM_14,              /**< user parameter slot 14                         */
    NV_PARAM_15,              /**< user parameter slot 15                         */
    NV_PARAM_16,              /**< user parameter slot 16                         */
    NV_PARAM_17,              /**< user parameter slot 17                         */
    NV_PARAM_18,              /**< user parameter slot 18                         */
    NV_PARAM_19,              /**< user parameter slot 19                         */
    NV_PARAM_20,              /**< user parameter slot 20                         */
    NV_PARAM_21,              /**< user parameter slot 21                         */
    NV_PARAM_22,              /**< user parameter slot 22                         */
    NV_PARAM_23,              /**< user parameter slot 23                         */
    NV_PARAM_24,              /**< user parameter slot 24                         */
    NV_PARAM_25,              /**< user parameter slot 25                         */
    NV_PARAM_26,              /**< user parameter slot 26                         */
    NV_PARAM_27,              /**< user parameter slot 27                         */
    NV_PARAM_28               /**< user parameter slot 28                         */
} NvParamId_t;

/**
 * @brief Load the parameter cache from FLASH at power-up. Validates the magic,
 *        version and CRC of the stored block; on any mismatch fills the cache
 *        with the firmware defaults. Does not write to FLASH.
 */
void  NvParams_Init(void);

/**
 * @brief Read a parameter from the RAM cache.
 * @param id  Parameter slot (0..NVM_PARAM_COUNT-1).
 * @return The cached value, or 0 if `id` is out of range.
 */
uint32_t NvParams_Get(uint8_t id);

/**
 * @brief Write a parameter to the RAM cache (RAM only; call #NvParams_Save to
 *        persist to FLASH).
 * @param id  Parameter slot (0..NVM_PARAM_COUNT-1).
 * @param val Value to store.
 */
void NvParams_Set(uint8_t id, uint32_t val);

/**
 * @brief Persist the RAM cache to FLASH: erase the parameter page and program
 *        the whole block (with recomputed CRC). Blocking, and disables IRQs
 *        around the flash access.
 * @return 0 on success, or a negative error code on failure.
 */
int   NvParams_Save(void);

/**
 * @brief Restore the RAM cache to the firmware defaults. Does not write to
 *        FLASH; call #NvParams_Save to persist them.
 */
void  NvParams_RestoreDefaults(void);

#ifdef __cplusplus
}
#endif

#endif /* __NV_PARAMS_H__ */