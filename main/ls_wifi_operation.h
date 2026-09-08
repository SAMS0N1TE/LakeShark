#ifndef LS_WIFI_OPERATION_H
#define LS_WIFI_OPERATION_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t owned; } ls_wifi_operation_t;

/* LS-761: Settings scans and console/AP commands share one radio. Never
 * wait on LVGL, and never let scan cleanup stop a newer operation's STA. */
static inline bool ls_wifi_operation_try(ls_wifi_operation_t *operation)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&operation->owned, &expected, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void ls_wifi_operation_end(ls_wifi_operation_t *operation)
{
    __atomic_store_n(&operation->owned, 0, __ATOMIC_RELEASE);
}

typedef int (*ls_wifi_operation_fn)(void *context);

static inline int ls_wifi_operation_run(ls_wifi_operation_t *operation,
    ls_wifi_operation_fn execute, void *context, int busy_error)
{
    if (!ls_wifi_operation_try(operation)) return busy_error;
    int result = execute(context);
    ls_wifi_operation_end(operation);
    return result;
}

/* No erase callback exists: a credential read must not repair the shared
 * NVS partition by destroying all of the radio's saved configuration. */
static inline int ls_wifi_nvs_prepare(bool *ready, int (*initialize)(void))
{
    if (*ready) return 0;
    int error = initialize();
    if (!error) *ready = true;
    return error;
}

#endif
