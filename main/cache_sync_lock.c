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
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 4)
static portMUX_TYPE s_cache_op_lock = portMUX_INITIALIZER_UNLOCKED;
#endif

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 4)
bool __real_cache_hal_invalidate_addr(uint32_t vaddr, uint32_t size);
bool __real_cache_hal_writeback_addr(uint32_t vaddr, uint32_t size);

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
#endif /* IDF 5.5.4 supplies the shared cache-operation lock itself. */

#if CONFIG_IDF_TARGET_ESP32P4
/* The whole data cache written back, under the same lock.

   The ELF loader (espressif/elf_loader 1.3.5) links every firmware module
   and SD app with two calls to the ROM's Cache_WriteBack_All from a task
   while the other core runs on: one after copying the segments
   (esp_elf_load_segment) and one in esp_elf_arch_flush, which declares the
   ROM call without its argument and so hands it the esp_elf_t pointer as
   the cache map. Neither takes the cache-operation lock, so a link could
   run its writeback straight through a ranged sync on the other core - the
   USB host's interrupt syncs every HackRF or RTL-SDR transfer, its DMA
   memory being in PSRAM. On 2026-11-25 the SX board panicked that way:
   core 1 had just linked MUSIC (its PC in the link task, on the link stack
   in LP RAM) while FM's stream was still running, and core 0 died in an
   interrupt releasing a critical section it no longer held (crumb #18).

   Every other caller asks for the L1 data cache (CACHE_MAP_L1_DCACHE): the
   cache suspend in spi_flash_disable_interrupts_caches_and_other_cpu, with
   the scheduler suspended and the other core parked, and esp_restart_noos,
   with interrupts off. Those keep running unlocked, as before: taking a lock
   there could wait on a core that is stopped. A map that is not a map (the
   loader's pointer) is the L1 data cache it means. */
#include "freertos/task.h"
#include "esp32p4/rom/cache.h"
#include "riscv/csr.h"
#include "cache_wb_policy.h"

int __real_Cache_WriteBack_All(uint32_t map);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 4)
extern void esp_cache_sync_ops_enter_critical_section(void);
extern void esp_cache_sync_ops_exit_critical_section(void);
#define CACHE_OP_LOCK() esp_cache_sync_ops_enter_critical_section()
#define CACHE_OP_UNLOCK() esp_cache_sync_ops_exit_critical_section()
#else
#define CACHE_OP_LOCK() portENTER_CRITICAL_SAFE(&s_cache_op_lock)
#define CACHE_OP_UNLOCK() portEXIT_CRITICAL_SAFE(&s_cache_op_lock)
#endif

int IRAM_ATTR __wrap_Cache_WriteBack_All(uint32_t map)
{
    map = cache_wb_map(map, CACHE_MAP_MASK, CACHE_MAP_L1_DCACHE);
    if (!cache_wb_takes_lock(xPortInIsrContext(), (RV_READ_CSR(mstatus) & MSTATUS_MIE) != 0,
                             xTaskGetSchedulerState() == taskSCHEDULER_RUNNING))
        return __real_Cache_WriteBack_All(map);
    CACHE_OP_LOCK();
    const int r = __real_Cache_WriteBack_All(map);
    CACHE_OP_UNLOCK();
    return r;
}
#endif /* CONFIG_IDF_TARGET_ESP32P4 */
