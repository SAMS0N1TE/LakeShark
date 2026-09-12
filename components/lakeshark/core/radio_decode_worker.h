#ifndef RADIO_DECODE_WORKER_H
#define RADIO_DECODE_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_RADIO_DECODE_STACK_BYTES 16384u

/* FM, P25 and ADS-B are mutually exclusive radio owners. Their cache-safe decode
 * work shares this one fixed internal stack, while each run retains its
 * original priority and core affinity. */
bool ls_radio_decode_worker_start(TaskFunction_t run, const char *name,
                                  UBaseType_t priority, BaseType_t core_id);

/* A run callback returns only after it has stopped touching its stack.  This
 * call waits for the wrapper task to reach the Suspended state, then deletes
 * its static TCB before making the storage available to the next owner. */
bool ls_radio_decode_worker_release(unsigned wait_iterations,
                                    uint32_t wait_ms);

#ifdef __cplusplus
}
#endif

#endif
