/* When a whole-cache writeback takes the cache-operation lock, and which
   caches it writes back (cache_sync_lock.c). Pure, so the bench checks it. */
#ifndef CACHE_WB_POLICY_H
#define CACHE_WB_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* A map with bits outside the ROM's cache map is not a map: the ELF loader
   passes its esp_elf_t pointer. It means the L1 data cache. */
static inline uint32_t cache_wb_map(uint32_t map, uint32_t map_mask, uint32_t l1_dcache)
{
    return (map & ~map_mask) ? l1_dcache : map;
}

/* Lock only where the other core runs on and can be inside a cache sync of
   its own: a task, with interrupts on and the scheduler running. With the
   scheduler suspended (the flash cache suspend, the other core parked) or
   interrupts off (restart, panic) the other core may be stopped holding the
   lock, and waiting for it would never end. */
static inline bool cache_wb_takes_lock(bool in_isr, bool irq_enabled, bool scheduler_running)
{
    return !in_isr && irq_enabled && scheduler_running;
}

#endif
