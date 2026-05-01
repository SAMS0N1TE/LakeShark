/*
 * diag_json.h — JSONL formatter, USB-Serial-JTAG sink.
 *
 * Wire format: \x1e<json>\n where <json> is a single self-contained
 * JSON object with at minimum {"ts":...,"tag":"..."}. The leading
 * \x1e (RFC 8742 record separator) lets a host that connects mid-
 * stream resync to the next record without parsing a partial JSON.
 *
 * Internal contract between diag_emit dispatch and the JSON sink.
 * Not part of the public producer API.
 */
#ifndef DIAG_JSON_H
#define DIAG_JSON_H

#include "diag_emit.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise USB-Serial-JTAG driver, ringbuf, drain task, RX cmd
 * task. Idempotent.
 */
void diag_json_init(void);

/* Format and enqueue one record. */
void diag_json_record(const char *tag, int n_fields, const diag_field_t *fields);

/* Drop counter accessor. */
uint32_t diag_json_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* DIAG_JSON_H */
