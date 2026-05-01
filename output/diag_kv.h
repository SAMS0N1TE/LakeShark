/*
 * diag_kv.h — key=value text formatter, UART1 sink.
 *
 * Internal contract between diag_emit dispatch and the kv sink. Not
 * part of the public producer API (use diag_emit() for that).
 */
#ifndef DIAG_KV_H
#define DIAG_KV_H

#include "diag_emit.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise UART1 driver, ringbuf, drain task. Idempotent. */
void diag_kv_init(void);

/* Format and enqueue one record. Synchronous up through ringbuf push;
 * actual UART transmission happens on the drain task.
 */
void diag_kv_record(const char *tag, int n_fields, const diag_field_t *fields);

/* Drop counter accessor for diag self-check. */
uint32_t diag_kv_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* DIAG_KV_H */
