/*
 * diag.h — telemetry API.
 *
 * NEW CODE: include "diag_emit.h" and use diag_emit() with typed
 * fields. The diag_emit API is the supported way forward.
 *
 * LEGACY CODE: keep using diag_line() / diag_vline() / diag_dump_nid().
 * These are thin wrappers around diag_emit() and will continue to work,
 * though they emit each printf line as a single string field (which is
 * uglier than typed but still parseable).
 *
 * This header pulls in diag_emit.h so a single #include "diag.h" gets
 * both APIs. New call sites should prefer the typed form.
 */
#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

/* New typed API. Always available. */
#include "diag_emit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sink configuration (compile-time) ────────────────────────
 *
 * These macros are defined in diag_kv.c / diag_json.c with weak
 * defaults; override them in sdkconfig if needed. The values here are
 * informational and match the defaults used by the sinks.
 */
#define DIAG_UART_NUM      1            /* UART1 */
#define DIAG_UART_TX_PIN   32           /* GPIO 32 (P25 diag wire-out) */
#define DIAG_UART_RX_PIN   33
#define DIAG_UART_BAUD     921600
#define DIAG_RING_BYTES    4096
#define DIAG_LINE_MAX      512

/* ── Lifecycle ────────────────────────────────────────────── */
/* Initialise both sinks. Idempotent. Equivalent to diag_emit_init(). */
void diag_init(void);

/* ── Legacy line output (printf-style) ────────────────────── */
/* Each call emits one record with a single "text" field carrying the
 * formatted body. Non-blocking; drops on ringbuf-full. CRLF added
 * by the kv sink, LF by the JSON sink.
 */
void diag_line (const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void diag_vline(const char *tag, const char *fmt, va_list ap);

/* ── Legacy NID dibit dump (now emits typed fields) ───────── */
void diag_dump_nid(const char *tag, const int *dibits33, int nac_raw,
                   const char *duid_raw, int ec, int verdict_ok,
                   const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* DIAG_H */
