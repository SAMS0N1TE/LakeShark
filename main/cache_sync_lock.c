/* One lock for every address-range cache operation.

   ESP-IDF 5.4.3 takes a lock around the cache syncs esp_cache_msync() runs
   for DMA buffers, but not around the invalidate the flash driver runs after
   every write or erase (spi_flash_check_and_flush_cache) or the one
   esp_mmu_map runs. Two of them at once on the ESP32-P4 can leave the cache
   waiting forever: on 2026-10-04 the LR2021 board's watchdog caught core 0
   in the ROM's Cache_Sync_Items under Cache_Op_Addr, with both cores' ticks
   stopped, twice (16:25, 19:26). ESP-IDF 5.5.4 fixes it by taking the
   msync lock in those two places (esp_cache_sync_ops_enter_critical_section);
   this does the same for 5.4.3 at link time, so every caller of
   cache_hal_invalidate_addr and cache_hal_writeback_addr is serialised.
   esp_cache_msync() already holds its own lock when it calls in; taking this
   one inside it never inverts, as nothing takes them the other way round. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"

bool __real_cache_hal_invalidate_addr(uint32_t vaddr, uint32_t size);
bool __real_cache_hal_writeback_addr(uint32_t vaddr, uint32_t size);

static portMUX_TYPE s_cache_op_lock = portMUX_INITIALIZER_UNLOCKED;

bool IRAM_ATTR __wrap_cache_hal_invalidate_addr(uint32_t vaddr, uint32_t size)
{
    portENTER_CRITICAL_SAFE(&s_cache_op_lock);
    const bool ok = __real_cache_hal_invalidate_addr(vaddr, size);
    portEXIT_CRITICAL_SAFE(&s_cache_op_lock);
    return ok;
}

bool IRAM_ATTR __wrap_cache_hal_writeback_addr(uint32_t vaddr, uint32_t size)
{
    portENTER_CRITICAL_SAFE(&s_cache_op_lock);
    const bool ok = __real_cache_hal_writeback_addr(vaddr, size);
    portEXIT_CRITICAL_SAFE(&s_cache_op_lock);
    return ok;
}
