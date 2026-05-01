/*
 * diag_emit.h — typed-field telemetry emit API.
 *
 * Single emit point. Fans out to two formatter sinks:
 *   diag_kv   → UART1 (GPIO 32 TX) at 921600 baud, key=value text
 *   diag_json → USB-Serial-JTAG, JSONL (\x1e<json>\n per record)
 *
 * Producers describe each field by type (int/uint/float/str/bool/hex/
 * int_array). Formatters consume the typed list and produce
 * format-appropriate output — no quoting bugs, no printf %d-on-pointer
 * mishaps, no drift between the two sinks.
 *
 * Wire format common to both sinks: every record carries
 *   ts        — float seconds since boot (monotonic, always present)
 *   dateTime  — RFC3339 wall-clock string, present iff a TIME message
 *               has been received from the host (see diag_time.c)
 *   tag       — short uppercase identifier (BOOT/RF/SYNC/HUNT/BCH/...)
 * plus zero or more typed fields.
 *
 * Thread-safe: callable from any task, ISR-safe is NOT a goal — call
 * from task context only. Drops on ringbuf-full are counted.
 */
#ifndef DIAG_EMIT_H
#define DIAG_EMIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Field type descriptor ───────────────────────────────────── */
typedef enum {
    DF_T_INT       = 0,   /* signed integer        → kv: %ld   json: number */
    DF_T_UINT      = 1,   /* unsigned integer      → kv: %lu   json: number */
    DF_T_FLOAT     = 2,   /* float/double          → kv: %.6g  json: number */
    DF_T_STR       = 3,   /* C string              → kv: bare* json: "quoted" */
    DF_T_BOOL      = 4,   /* boolean               → kv: 0/1   json: true/false */
    DF_T_HEX       = 5,   /* hex integer           → kv: 0xNN  json: number   */
    DF_T_INT_ARRAY = 6,   /* int[]                 → kv: a,b,c json: [a,b,c]  */
} diag_ftype_t;

/* (*) Strings in kv are emitted bare unless they contain whitespace,
 * '=', or non-printable chars; in that case the kv formatter falls
 * back to "double-quoted" and escapes embedded quotes/backslashes.
 * JSON always quotes. */

typedef struct {
    const char    *key;
    diag_ftype_t   type;
    union {
        long           i;
        unsigned long  u;
        double         f;
        const char    *s;
        bool           b;
        struct { const int *arr; int n; } a;
    } v;
} diag_field_t;

/* Convenience constructors. Producers compose a compound literal:
 *
 *   diag_emit("CALL", 4, (diag_field_t[]){
 *       DF_STR ("event",     "call_start"),
 *       DF_HEX ("nac",       0x293),
 *       DF_INT ("tg",        51),
 *       DF_BOOL("encrypted", false),
 *   });
 *
 * The C99 compound literal lifetime is the enclosing block — diag_emit
 * MUST consume it synchronously, which it does (formats and pushes to
 * ringbufs before returning). Strings referenced via DF_STR must
 * remain valid until diag_emit returns; passing string literals or
 * stack buffers used in the same expression is fine.
 */
#define DF_INT(k, val)    ((diag_field_t){.key=(k), .type=DF_T_INT,       .v.i=(long)(val)})
#define DF_UINT(k, val)   ((diag_field_t){.key=(k), .type=DF_T_UINT,      .v.u=(unsigned long)(val)})
#define DF_FLT(k, val)    ((diag_field_t){.key=(k), .type=DF_T_FLOAT,     .v.f=(double)(val)})
#define DF_STR(k, val)    ((diag_field_t){.key=(k), .type=DF_T_STR,       .v.s=(val)})
#define DF_BOOL(k, val)   ((diag_field_t){.key=(k), .type=DF_T_BOOL,      .v.b=(bool)(val)})
#define DF_HEX(k, val)    ((diag_field_t){.key=(k), .type=DF_T_HEX,       .v.u=(unsigned long)(val)})
#define DF_ARR(k, p, len) ((diag_field_t){.key=(k), .type=DF_T_INT_ARRAY, .v.a={.arr=(p), .n=(len)}})

/* ── Lifecycle ───────────────────────────────────────────────── */
/* Initialise both sinks. Idempotent. Safe to call before app_main
 * has started other subsystems — no dependencies on USB host or
 * SDR state. Both sink drain tasks are pinned to core 1 to avoid
 * fighting USB host code on core 0.
 */
void diag_emit_init(void);

/* ── Core emit ───────────────────────────────────────────────── */
/* Synchronous: formats the line into both sinks' ringbufs before
 * returning. Returns immediately if formatting succeeds; drops are
 * tallied internally if a sink's ring is full.
 *
 * tag      — short uppercase string, ≤8 chars recommended
 * n_fields — number of fields in the array (may be 0)
 * fields   — array of typed fields. Lifetime must extend until call
 *            returns (compound literals are fine).
 */
void diag_emit(const char *tag, int n_fields, const diag_field_t *fields);

/* ── Counter accessors ───────────────────────────────────────── */
/* These are still here because hot paths (dsd_frame_sync, BCH check)
 * call them by value many times per second; we accumulate in shared
 * counter state and emit a 1-Hz rollup via diag_emit_periodic().
 *
 * Implementations live in diag_emit.c (the new home for the periodic
 * task and counter state). The signatures are unchanged from the old
 * diag.h for source-compat with existing callers.
 */
void diag_count_sync_attempt(int matched_exact, int best_hd);
void diag_count_bch_result  (int ok, int ec);          /* ec 0..11 ok, -1 fail */
void diag_count_frame       (const char *duid_two_chars);

/* ── Periodic 1-Hz rollup ────────────────────────────────────── */
/* Call from any task on a regular cadence; internal rate-limit
 * ensures we emit at most once per second. Safe to call too often.
 */
void diag_emit_periodic(void);

/* ── Helpers ─────────────────────────────────────────────────── */
float diag_uptime_s(void);

/* Drop counters for the two sinks. Read-only; useful for diag self-
 * check from the periodic task. Reset by any successful drain.
 */
uint32_t diag_kv_dropped  (void);
uint32_t diag_json_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* DIAG_EMIT_H */
