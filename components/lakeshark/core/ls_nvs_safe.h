#pragma once

/* LS-671: run NVS work from a task whose stack is definitely in internal RAM.
 *
 * An NVS read or write ends in spi_flash_disable_interrupts_caches_and_other_cpu(),
 * which asserts esp_task_stack_is_sane_cache_disabled(). With the cache off a
 * stack in external RAM cannot be reached, so a task whose stack is in PSRAM
 * must not call into NVS at all:
 *
 *   assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
 *   cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())
 *
 * This build makes that easy to hit. Two task stacks are declared
 * EXT_RAM_BSS_ATTR outright - p25_rx and the scan engine - and
 * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY is enabled, so a dynamically
 * created task can be given an external stack too. Any of those calling NVS
 * panics at boot, and it did: once from the P25 scan-list persistence and once
 * from the BLE pinned-peer load.
 *
 * Rather than move large stacks into scarce internal RAM, move the flash
 * access: ls_nvs_call() runs one callback on a short-lived task whose stack is
 * explicitly allocated from DMA-capable internal DRAM and blocks the caller
 * until it returns. MALLOC_CAP_INTERNAL alone is insufficient on ESP32-P4:
 * its 0x30100000 TCM heap has that capability, while IDF 5.5.4's cache-off
 * assertion accepts only 0x4ff00000-0x4ffc0000 DRAM (and optionally RTC fast
 * RAM). The caller keeps whatever stack it has and never has the cache pulled
 * from under it.
 *
 * Only the flash access needs to move. Serialising a blob, formatting a
 * string, deciding what to store - all of that is ordinary memory work and
 * belongs on the caller.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The work to perform. Return an esp_err_t; it is passed back to the caller. */
typedef esp_err_t (*ls_nvs_fn_t)(void *ctx);

/* The cache-safe worker has this exact statically reserved DRAM stack. */
#define LS_NVS_WORKER_STACK_BYTES 3072u

/* Prepare the static dispatch lock. Call once during early startup, before
 * independent subsystems can issue NVS work. No heap allocation is performed;
 * repeated calls are harmless. */
esp_err_t ls_nvs_init(void);

/* Runs fn(ctx) on a cache-safe DRAM-stack task and waits for it.
 *
 * Returns whatever fn returned, ESP_ERR_INVALID_STATE when ls_nvs_init() was
 * not called, ESP_ERR_INVALID_SIZE for a stack request larger than the fixed
 * reservation, or ESP_ERR_NO_MEM if static task creation fails. Failing to
 * persist a setting is not worth panicking over, so callers should degrade
 * rather than abort.
 *
 * stack_bytes bounds the worker, as ESP-IDF task APIs do. Keep it small: it
 * exists only to hold an NVS handle and move a blob. The default is exactly
 * LS_NVS_WORKER_STACK_BYTES; pass 0 for that default. Requests may be smaller
 * but never larger than the reserved stack. Do not do heavy work inside fn.
 *
 * Safe to call from any task, including one with an external stack. Not safe
 * from an ISR, and fn must not call ls_nvs_call() recursively. Concurrent
 * callers are serialized onto the single reserved stack. */
esp_err_t ls_nvs_call(ls_nvs_fn_t fn, void *ctx, unsigned stack_bytes);

#ifdef __cplusplus
}
#endif
