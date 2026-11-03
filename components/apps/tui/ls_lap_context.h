/* API 2.6 services: task-local selection, bounded per-instance resource tables.
 * Slot storage outlives callbacks (including late DNS completions). Workers
 * explicitly select an instance; producers use the owning lease's slot. */
#ifndef LS_LAP_CONTEXT_H
#define LS_LAP_CONTEXT_H
#include <stdbool.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_attr.h"
#define LS_LAP_POOL EXT_RAM_BSS_ATTR
#else
#define LS_LAP_POOL
#endif
/* Per board in main/Kconfig.projbuild; this is the host harnesses' default. */
#ifndef CONFIG_LS_LAP_SERVICES_MAX
#define CONFIG_LS_LAP_SERVICES_MAX 3
#endif
#define LS_LAP_CONTEXTS (CONFIG_LS_LAP_SERVICES_MAX + 1)
/* Weak fallback permits focused subsystem harnesses without the ELF runtime. */
#ifdef LS_LAP_CONTEXT_IMPLEMENTATION
#define LS_LAP_OPTIONAL
#else
#define LS_LAP_OPTIONAL __attribute__((weak))
#endif
unsigned ls_lap_instance_slot(void) LS_LAP_OPTIONAL;
static inline unsigned ls_lap_context_index(void) {
#ifdef LS_LAP_CONTEXT_IMPLEMENTATION
    return ls_lap_instance_slot();
#else
    return ls_lap_instance_slot ? ls_lap_instance_slot() : 0;
#endif
}
enum { LS_LAP_LEASE_IQ, LS_LAP_LEASE_PACKET, LS_LAP_LEASE_BLE, LS_LAP_LEASE_MIC, LS_LAP_LEASE_NFC, LS_LAP_LEASE_LEGACY_RADIO, LS_LAP_LEASE_COUNT };
bool ls_lap_resource_claim_impl(unsigned kind, bool acquire) LS_LAP_OPTIONAL;
static inline bool ls_lap_resource_claim(unsigned kind, bool acquire) {
#ifdef LS_LAP_CONTEXT_IMPLEMENTATION
    return ls_lap_resource_claim_impl(kind,acquire);
#else
    return !ls_lap_resource_claim_impl || ls_lap_resource_claim_impl(kind, acquire);
#endif
}
unsigned ls_lap_producer_select_impl(unsigned kind) LS_LAP_OPTIONAL;
void ls_lap_producer_restore_impl(unsigned slot) LS_LAP_OPTIONAL;
static inline unsigned ls_lap_producer_select(unsigned kind) {
#ifdef LS_LAP_CONTEXT_IMPLEMENTATION
    return ls_lap_producer_select_impl(kind);
#else
    return ls_lap_producer_select_impl?ls_lap_producer_select_impl(kind):0;
#endif
}
static inline void ls_lap_producer_restore(unsigned slot) {
#ifdef LS_LAP_CONTEXT_IMPLEMENTATION
    ls_lap_producer_restore_impl(slot);
#else
    if(ls_lap_producer_restore_impl) ls_lap_producer_restore_impl(slot);
#endif
}
#endif
