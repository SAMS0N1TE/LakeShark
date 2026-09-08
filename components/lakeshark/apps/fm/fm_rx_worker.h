#ifndef FM_RX_WORKER_H
#define FM_RX_WORKER_H

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "radio_decode_worker.h"

#define FM_RX_STACK_BYTES LS_RADIO_DECODE_STACK_BYTES

/* Start one FM lifecycle run on the shared cache-safe radio decode stack. */
bool fm_rx_worker_start(TaskFunction_t worker);

/* Release the stack only after the FM run has returned and the wrapper is
 * observably suspended. */
bool fm_rx_worker_release(unsigned wait_iterations, uint32_t wait_ms);

#ifdef __cplusplus
}
#endif

#endif
