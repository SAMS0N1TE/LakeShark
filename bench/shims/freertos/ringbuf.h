/* Host shim. Minimal ring-buffer declarations for compiling P25 diagnostics;
   tests provide the behavior they need. */
#ifndef LS_SHIM_RINGBUF_H
#define LS_SHIM_RINGBUF_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *RingbufHandle_t;

typedef enum {
    RINGBUF_TYPE_NOSPLIT = 0,
    RINGBUF_TYPE_ALLOWSPLIT,
    RINGBUF_TYPE_BYTEBUF,
} RingbufferType_t;

RingbufHandle_t xRingbufferCreate(size_t buffer_size,
                                  RingbufferType_t buffer_type);
BaseType_t xRingbufferSend(RingbufHandle_t ring, const void *item,
                           size_t item_size, TickType_t ticks);
void *xRingbufferReceive(RingbufHandle_t ring, size_t *item_size,
                         TickType_t ticks);
void vRingbufferReturnItem(RingbufHandle_t ring, void *item);

#ifdef __cplusplus
}
#endif

#endif
