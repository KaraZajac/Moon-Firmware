#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum size of the XIP flash region (280KB = 70 pages of 4KB).
 *  Sized to fit alongside the BLE Full Extended stack (47 pages)
 *  with a small margin before the radio region. */
#define XIP_REGION_MAX_SIZE (280 * 1024)

/** Cache header magic value ("XIPC") */
#define XIP_CACHE_MAGIC 0x58495043

/** Maximum cached sections */
#define XIP_CACHE_MAX_SECTIONS 8

/** Size reserved at start of XIP region for cache header (8-byte aligned) */
#define XIP_CACHE_HEADER_SIZE 256

/** Per-section cache entry */
typedef struct {
    uint32_t flash_offset; /**< Offset from XIP base where section data starts */
    uint32_t size; /**< Section size in bytes */
    char name[16]; /**< Section name (".text", ".rodata", etc.) */
} XipCacheSectionEntry;

/** Cache header stored at the start of the XIP flash region.
 *  Allows skipping erase/write when re-launching the same app.
 */
typedef struct {
    uint32_t magic; /**< XIP_CACHE_MAGIC */
    uint32_t file_size; /**< FAP file size (quick validation) */
    uint32_t file_crc32; /**< CRC32 of FAP file (definitive validation) */
    uint32_t api_version; /**< Firmware API version (major << 16 | minor) */
    uint32_t section_count; /**< Number of cached sections */
    XipCacheSectionEntry sections[XIP_CACHE_MAX_SECTIONS];
} XipCacheHeader;

/** XIP flash region bump allocator.
 *  Manages a region of internal flash used for execute-in-place loading
 *  of FAP .text and .rodata sections.
 */
typedef struct {
    uint32_t base_addr; /**< Flash start address (page-aligned) */
    uint32_t end_addr; /**< Flash end address */
    uint32_t data_start; /**< Where section data begins (after cache header) */
    uint32_t next_free; /**< Next available address (bump pointer) */
    bool active; /**< Whether XIP is available */
    bool cache_valid; /**< True if cached XIP data matches current app */
} XipRegion;

/** Initialize XIP region from free flash.
 *  Reserves space for the cache header at the start.
 *  Queries the free flash area and checks if at least XIP_REGION_MAX_SIZE
 *  bytes are available. Sets region->active = true on success.
 *
 *  @param region   XIP region to initialize
 */
void xip_region_init(XipRegion* region);

/** Validate the XIP cache against the current FAP file.
 *  Reads the cache header from flash and compares file size, CRC, and API version.
 *
 *  @param region           XIP region (must be initialized)
 *  @param fd               open file handle to the FAP file
 *  @param api_version_major firmware API major version
 *  @param api_version_minor firmware API minor version
 *  @return                 true if cache is valid (can skip erase/write)
 */
bool xip_cache_validate(
    XipRegion* region,
    File* fd,
    uint16_t api_version_major,
    uint16_t api_version_minor);

/** Write a pre-built cache header to the XIP flash region.
 *  The header area must have been erased as part of the XIP erase cycle.
 *
 *  @param region   XIP region
 *  @param header   fully-populated cache header to write
 *  @return         true on success
 */
bool xip_cache_commit_header(XipRegion* region, const XipCacheHeader* header);

/** Get the cached header from flash (read-only, memory-mapped).
 *
 *  @param region   XIP region
 *  @return         pointer to flash-resident header, or NULL if not active
 */
const XipCacheHeader* xip_cache_get_header(const XipRegion* region);

/** Allocate address space from the XIP region (bump allocator).
 *  Does NOT erase or write flash — just advances the pointer.
 *  Alignment is rounded up to 8 (flash write block size) minimum.
 *
 *  @param region       XIP region
 *  @param size         bytes to allocate
 *  @param alignment    required alignment (will be clamped to >= 8)
 *  @return             flash address, or 0 on failure
 */
uint32_t xip_region_alloc(XipRegion* region, size_t size, size_t alignment);

/** Erase only the flash pages that have been allocated.
 *  Must be called after all xip_region_alloc() calls and before
 *  xip_region_commit() calls.
 *
 *  @param region   XIP region
 *  @return         true on success
 */
bool xip_region_erase(XipRegion* region);

/** Write RAM staging buffer to pre-erased flash.
 *
 *  @param region       XIP region (for bounds checking)
 *  @param flash_addr   destination address in flash (must be 8-byte aligned)
 *  @param ram_data     source data in RAM
 *  @param size         number of bytes to write
 *  @return             true on success
 */
bool xip_region_commit(XipRegion* region, uint32_t flash_addr, const void* ram_data, size_t size);

/** Release the XIP region so another app can use it.
 *  Called when the app using XIP is freed.
 *
 *  @param region   XIP region to release
 */
void xip_region_release(XipRegion* region);

/** Get total bytes allocated so far.
 *
 *  @param region   XIP region
 *  @return         bytes used
 */
size_t xip_region_used(const XipRegion* region);

#ifdef __cplusplus
}
#endif
