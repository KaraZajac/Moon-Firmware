#include "elf_file_xip.h"
#include <furi_hal_flash.h>
#include <furi.h>
#include <toolbox/crc32_calc.h>

#define TAG "XIP"

static bool xip_region_in_use = false;

void xip_region_init(XipRegion* region) {
    furi_check(region);
    memset(region, 0, sizeof(XipRegion));

    if(xip_region_in_use) {
        FURI_LOG_W(TAG, "XIP region already in use by another app, skipping");
        return;
    }

    size_t free_start = furi_hal_flash_get_free_page_start_address();
    size_t free_end = (size_t)furi_hal_flash_get_free_end_address();

    if(free_end <= free_start) {
        FURI_LOG_W(TAG, "No free flash available");
        return;
    }

    size_t free_size = free_end - free_start;
    if(free_size < XIP_REGION_MAX_SIZE) {
        FURI_LOG_W(
            TAG,
            "Not enough free flash for XIP: %zu available, %u needed",
            free_size,
            XIP_REGION_MAX_SIZE);
        return;
    }

    region->base_addr = (uint32_t)free_start;
    region->end_addr = region->base_addr + XIP_REGION_MAX_SIZE;
    /* Reserve space for cache header at start of region */
    region->data_start = region->base_addr + XIP_CACHE_HEADER_SIZE;
    region->next_free = region->data_start;
    region->active = true;
    region->cache_valid = false;
    xip_region_in_use = true;

    FURI_LOG_I(
        TAG,
        "Region initialized: 0x%08lX - 0x%08lX (%u KB, header at base)",
        region->base_addr,
        region->end_addr,
        XIP_REGION_MAX_SIZE / 1024);
}

void xip_region_release(XipRegion* region) {
    furi_check(region);
    if(region->active) {
        xip_region_in_use = false;
        region->active = false;
        FURI_LOG_D(TAG, "Region released");
    }
}

bool xip_cache_validate(
    XipRegion* region,
    File* fd,
    uint16_t api_version_major,
    uint16_t api_version_minor) {
    furi_check(region);
    furi_check(fd);

    if(!region->active) return false;

    /* Read cache header directly from flash (memory-mapped on STM32) */
    const XipCacheHeader* header = (const XipCacheHeader*)(region->base_addr);

    /* Tier 1: Quick checks (instant, no I/O) */
    if(header->magic != XIP_CACHE_MAGIC) {
        FURI_LOG_I(TAG, "Cache miss: no valid header (magic=0x%08lX)", header->magic);
        return false;
    }

    uint32_t current_api = ((uint32_t)api_version_major << 16) | api_version_minor;
    if(header->api_version != current_api) {
        FURI_LOG_I(
            TAG,
            "Cache miss: API version mismatch (cached=%08lX, current=%08lX)",
            header->api_version,
            current_api);
        return false;
    }

    uint64_t file_size = storage_file_size(fd);
    if(header->file_size != (uint32_t)file_size) {
        FURI_LOG_I(
            TAG,
            "Cache miss: file size mismatch (cached=%lu, current=%llu)",
            header->file_size,
            file_size);
        return false;
    }

    if(header->section_count == 0 || header->section_count > XIP_CACHE_MAX_SECTIONS) {
        FURI_LOG_I(TAG, "Cache miss: invalid section count %lu", header->section_count);
        return false;
    }

    /* Tier 2: CRC check (requires reading entire FAP file, ~50-80ms) */
    uint32_t tick_start = furi_get_tick();
    uint32_t file_crc = crc32_calc_file(fd, NULL, NULL);
    storage_file_seek(fd, 0, true); /* Reset position after full-file read */
    uint32_t crc_ms = furi_get_tick() - tick_start;

    if(header->file_crc32 != file_crc) {
        FURI_LOG_I(
            TAG,
            "Cache miss: CRC mismatch (cached=%08lX, computed=%08lX, %lums)",
            header->file_crc32,
            file_crc,
            crc_ms);
        return false;
    }

    FURI_LOG_I(
        TAG,
        "Cache HIT: %lu sections, CRC %08lX verified in %lums",
        header->section_count,
        file_crc,
        crc_ms);

    region->cache_valid = true;
    return true;
}

/** Write a pre-built cache header to the XIP flash region.
 *  The header area (first 256 bytes) must already be erased.
 */
bool xip_cache_commit_header(XipRegion* region, const XipCacheHeader* header) {
    furi_check(region);
    furi_check(header);

    if(!region->active) return false;

    /* Write header to the reserved area at region base.
     * This area was erased as part of xip_region_erase (it's the first page). */
    furi_hal_flash_write_block(
        region->base_addr, (const uint8_t*)header, sizeof(XipCacheHeader));

    FURI_LOG_I(
        TAG,
        "Cache header written: %lu sections, CRC %08lX",
        header->section_count,
        header->file_crc32);

    return true;
}

/** Get the cached header from flash (read-only, memory-mapped). */
const XipCacheHeader* xip_cache_get_header(const XipRegion* region) {
    if(!region->active) return NULL;
    return (const XipCacheHeader*)(region->base_addr);
}

uint32_t xip_region_alloc(XipRegion* region, size_t size, size_t alignment) {
    furi_check(region);
    if(!region->active || size == 0) return 0;

    /* Flash writes require 8-byte alignment minimum */
    if(alignment < 8) alignment = 8;

    /* Align the bump pointer */
    uint32_t aligned = (region->next_free + alignment - 1) & ~(alignment - 1);

    if(aligned + size > region->end_addr) {
        FURI_LOG_E(
            TAG,
            "Alloc failed: need %zu at 0x%08lX, end 0x%08lX",
            size,
            aligned,
            region->end_addr);
        return 0;
    }

    region->next_free = aligned + size;

    FURI_LOG_D(TAG, "Alloc %zu bytes at 0x%08lX", size, aligned);
    return aligned;
}

bool xip_region_erase(XipRegion* region) {
    furi_check(region);
    if(!region->active) return false;

    /* Erase from base_addr (including header area) to next_free */
    if(region->next_free == region->data_start) return true; /* nothing allocated */

    size_t page_size = furi_hal_flash_get_page_size();
    int16_t first_page = furi_hal_flash_get_page_number(region->base_addr);
    int16_t last_page = furi_hal_flash_get_page_number(region->next_free - 1);

    furi_check(first_page >= 0);
    furi_check(last_page >= 0);

    FURI_LOG_I(
        TAG, "Erasing pages %d - %d (%zu KB)", first_page, last_page,
        ((last_page - first_page + 1) * page_size) / 1024);

    for(int16_t page = first_page; page <= last_page; page++) {
        furi_hal_flash_erase(page);
        /* Yield every 4 pages to let BLE stack process events. */
        if((page - first_page) % 4 == 3) {
            furi_delay_tick(1);
        }
    }

    return true;
}

bool xip_region_commit(
    XipRegion* region,
    uint32_t flash_addr,
    const void* ram_data,
    size_t size) {
    furi_check(region);
    furi_check(ram_data);

    if(!region->active) return false;
    if(size == 0) return true;

    /* Bounds check */
    if(flash_addr < region->base_addr || (flash_addr + size) > region->end_addr) {
        FURI_LOG_E(TAG, "Commit out of bounds: 0x%08lX + %zu", flash_addr, size);
        return false;
    }

    /* Alignment check */
    if(flash_addr & 0x7) {
        FURI_LOG_E(TAG, "Commit address not 8-byte aligned: 0x%08lX", flash_addr);
        return false;
    }

    /* Use bulk write for speed — single begin/end cycle */
    furi_hal_flash_write_block(flash_addr, (const uint8_t*)ram_data, size);

    FURI_LOG_D(TAG, "Committed %zu bytes to 0x%08lX", size, flash_addr);
    return true;
}

size_t xip_region_used(const XipRegion* region) {
    furi_check(region);
    if(!region->active) return 0;
    return region->next_free - region->data_start;
}
