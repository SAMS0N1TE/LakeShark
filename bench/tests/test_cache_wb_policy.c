/* The whole-cache writeback wrapper (main/cache_sync_lock.c): the ELF
   loader's pointer-as-map becomes the L1 data cache, and the cache-operation
   lock is taken only where the other core runs on (crumb #18, 2026-11-25). */
#include "ls_test.h"
#include "cache_wb_policy.h"

/* esp32p4/rom/cache.h */
#define MAP_L1_ICACHE_0 (1u << 0)
#define MAP_L1_ICACHE_1 (1u << 1)
#define MAP_L1_DCACHE   (1u << 4)
#define MAP_L2_CACHE    (1u << 5)
#define MAP_MASK (MAP_L1_ICACHE_0 | MAP_L1_ICACHE_1 | MAP_L1_DCACHE | MAP_L2_CACHE)

LS_CASE(real_maps_pass_through)
{
    LS_EQ_UINT(cache_wb_map(MAP_L1_DCACHE, MAP_MASK, MAP_L1_DCACHE), MAP_L1_DCACHE);
    LS_EQ_UINT(cache_wb_map(MAP_L2_CACHE, MAP_MASK, MAP_L1_DCACHE), MAP_L2_CACHE);
    LS_EQ_UINT(cache_wb_map(MAP_L1_DCACHE | MAP_L2_CACHE, MAP_MASK, MAP_L1_DCACHE),
               MAP_L1_DCACHE | MAP_L2_CACHE);
}

LS_CASE(loader_pointer_means_l1_data_cache)
{
    /* esp_elf_t in DRAM and in PSRAM: both carry bits no map has. */
    LS_EQ_UINT(cache_wb_map(0x4ff4cbc0u, MAP_MASK, MAP_L1_DCACHE), MAP_L1_DCACHE);
    LS_EQ_UINT(cache_wb_map(0x48f5a030u, MAP_MASK, MAP_L1_DCACHE), MAP_L1_DCACHE);
    LS_EQ_UINT(cache_wb_map(0x40u, MAP_MASK, MAP_L1_DCACHE), MAP_L1_DCACHE);
}

LS_CASE(lock_only_where_the_other_core_runs)
{
    /* A task linking a module: lock. */
    LS_CHECK(cache_wb_takes_lock(false, true, true));
    /* Flash cache suspend: scheduler suspended, other core parked. */
    LS_CHECK(!cache_wb_takes_lock(false, true, false));
    /* esp_restart_noos and the panic path: interrupts off. */
    LS_CHECK(!cache_wb_takes_lock(false, false, true));
    LS_CHECK(!cache_wb_takes_lock(false, false, false));
    /* An interrupt never waits on it. */
    LS_CHECK(!cache_wb_takes_lock(true, true, true));
}
