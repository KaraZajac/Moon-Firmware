#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FURI_HAL_FLASH_OB_RAW_SIZE_BYTES 0x80
#define FURI_HAL_FLASH_OB_SIZE_WORDS     (FURI_HAL_FLASH_OB_RAW_SIZE_BYTES / sizeof(uint32_t))
#define FURI_HAL_FLASH_OB_TOTAL_VALUES   (FURI_HAL_FLASH_OB_SIZE_WORDS / 2)

typedef union {
    uint8_t bytes[FURI_HAL_FLASH_OB_RAW_SIZE_BYTES];
    union {
        struct {
            uint32_t base;
            uint32_t complementary_value;
        } values;
        uint64_t dword;
    } obs[FURI_HAL_FLASH_OB_TOTAL_VALUES];
} FuriHalFlashRawOptionByteData;

_Static_assert(
    sizeof(FuriHalFlashRawOptionByteData) == FURI_HAL_FLASH_OB_RAW_SIZE_BYTES,
    "UpdateManifestOptionByteData size error");

/** Init flash, applying necessary workarounds
 */
void furi_hal_flash_init(void);

/** Get flash base address
 *
 * @return     pointer to flash base
 */
size_t furi_hal_flash_get_base(void);

/** Get flash read block size
 *
 * @return     size in bytes
 */
size_t furi_hal_flash_get_read_block_size(void);

/** Get flash write block size
 *
 * @return     size in bytes
 */
size_t furi_hal_flash_get_write_block_size(void);

/** Get flash page size
 *
 * @return     size in bytes
 */
size_t furi_hal_flash_get_page_size(void);

/** Get expected flash cycles count
 *
 * @return     count of erase-write operations
 */
size_t furi_hal_flash_get_cycles_count(void);

/** Get free flash start address
 *
 * @return     pointer to free region start
 */
const void* furi_hal_flash_get_free_start_address(void);

/** Get free flash end address
 *
 * @return     pointer to free region end
 */
const void* furi_hal_flash_get_free_end_address(void);

/** Get first free page start address
 *
 * @return     first free page memory address
 */
size_t furi_hal_flash_get_free_page_start_address(void);

/** Get free page count
 *
 * @return     free page count
 */
size_t furi_hal_flash_get_free_page_count(void);

/** Erase Flash
 *
 * @warning    locking operation with critical section, stalls execution
 *
 * @param      page  The page to erase
 */
void furi_hal_flash_erase(uint8_t page);

/** Write double word (64 bits)
 *
 * @warning locking operation with critical section, stalls execution
 *
 * @param      address  destination address, must be double word aligned.
 * @param      data     data to write
 */
void furi_hal_flash_write_dword(size_t address, uint64_t data);

/** Write aligned page data (up to page size)
 *
 * @warning locking operation with critical section, stalls execution
 *
 * @param      address  destination address, must be page aligned.
 * @param      data     data to write
 * @param      length   data length
 */
void furi_hal_flash_program_page(const uint8_t page, const uint8_t* data, uint16_t length);

/** Write block of data to pre-erased flash using fast programming
 *
 * @warning locking operation with critical section, stalls execution.
 *          Flash must be erased before calling this function.
 *
 * @param      address  destination address, must be 8-byte aligned
 * @param      data     source data
 * @param      length   number of bytes to write (handles non-8-byte-aligned tail)
 */
void furi_hal_flash_write_block(size_t address, const uint8_t* data, size_t length);

/** Get flash page number for address
 *
 * @return     page number, -1 for invalid address
 */
int16_t furi_hal_flash_get_page_number(size_t address);

/** Writes OB word, using non-compl. index of register in Flash, OPTION_BYTE_BASE
 *
 * @warning locking operation with critical section, stalls execution
 *
 * @param      word_idx  OB word number
 * @param      value    data to write
 * @return     true if value was written, false for read-only word
 */
bool furi_hal_flash_ob_set_word(size_t word_idx, const uint32_t value);

/** Forces a reload of OB data from flash to registers
 *
 * @warning Initializes system restart
 *
 */
void furi_hal_flash_ob_apply(void);

/** Get raw OB storage data
 *
 * @return     pointer to read-only data of OB (raw + complementary values)
 */
const FuriHalFlashRawOptionByteData* furi_hal_flash_ob_get_raw_ptr(void);

/** Flush instruction and data caches
 *
 * Must be called after writing executable code to flash
 * to ensure the CPU fetches the updated content.
 */
void furi_hal_flash_flush_cache(void);

/** Begin a batch of flash operations.
 *
 * Acquires the Core2 mutex and sends a single SHCI_C2_FLASH_EraseActivity(ON)
 * notification. All subsequent furi_hal_flash_erase / furi_hal_flash_write_block
 * calls will skip per-operation Core2 locking while the batch is active.
 *
 * Must be paired with furi_hal_flash_batch_end().
 * BLE operations are paused for the duration of the batch.
 */
void furi_hal_flash_batch_begin(void);

/** End a batch of flash operations.
 *
 * Sends SHCI_C2_FLASH_EraseActivity(OFF) and releases the Core2 mutex.
 * BLE operations resume after this call.
 */
void furi_hal_flash_batch_end(void);

/** Check if a flash batch is currently active.
 * @return true if between batch_begin and batch_end
 */
bool furi_hal_flash_batch_is_active(void);

/** Prevent flash operations while ISR-callable code executes from flash.
 *
 * Acquires Core2 mutex and tells BLE stack to defer flash writes.
 * Use when DMA/timer ISR callbacks execute code from XIP flash —
 * prevents Core2 flash operations from stalling instruction fetch.
 *
 * Safe to call if already protected (no-op).
 * Must be paired with furi_hal_flash_unprotect_during_execution().
 */
void furi_hal_flash_protect_during_execution(void);

/** Resume normal flash operations after ISR-critical section.
 *
 * Releases Core2 mutex and tells BLE stack to resume flash writes.
 * Safe to call if not protected (no-op).
 */
void furi_hal_flash_unprotect_during_execution(void);

#ifdef __cplusplus
}
#endif
