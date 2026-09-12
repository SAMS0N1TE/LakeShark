#pragma once

/* run NVS work from a task whose stack is definitely in internal RAM. */

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

/* Runs fn(ctx) on a cache-safe DRAM-stack task and waits for it. */

esp_err_t ls_nvs_call(ls_nvs_fn_t fn, void *ctx, unsigned stack_bytes);

#ifdef __cplusplus
}
#endif
