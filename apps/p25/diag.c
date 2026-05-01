/*
 * diag.c — legacy compat shim over diag_emit.
 *
 * The new typed-field telemetry API lives in diag_emit.h / diag_emit.c.
 * This file keeps the OLD function names working while we migrate
 * call sites one at a time. Eventually delete this file when nobody
 * calls diag_line / diag_vline / diag_dump_nid anymore.
 *
 * What stays in the old shim:
 *   diag_line   — variadic printf wrapper, emits as a single text= field
 *   diag_vline  — same with va_list
 *   diag_dump_nid — NID dibit dump, structured into typed fields
 *   diag_init   — calls diag_emit_init() (moved there)
 *
 * The counter functions (diag_count_*) and the periodic emit
 * (diag_emit_periodic) moved to diag_emit.c and are no longer
 * defined here.
 */
#include "diag.h"
#include "diag_emit.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ── lifecycle ─────────────────────────────────────────── */
void diag_init(void)
{
    /* Forward to new module. Idempotent. */
    diag_emit_init();
}

/* ── legacy line output ────────────────────────────────── */
void diag_vline(const char *tag, const char *fmt, va_list ap)
{
    char body[DIAG_LINE_MAX];
    int  m = vsnprintf(body, sizeof(body), fmt, ap);
    if (m < 0) return;
    if (m >= (int)sizeof(body)) m = (int)sizeof(body) - 1;
    body[m] = 0;

    /* Emit as a single text field. The kv sink will quote it because
     * it contains spaces; the JSON sink always quotes strings. The
     * result is grep-friendly in both. Once a producer migrates to
     * typed fields, this wrapper becomes a no-op for that producer.
     */
    diag_emit(tag, 1, (diag_field_t[]){
        DF_STR("text", body),
    });

#ifdef CONFIG_ENABLE_TUI
    /* TUI mirror: when the TUI is compiled in, mirror selected tags
     * to the LOG page so the developer sees frame-sync diagnostics
     * without hooking up the diag UART. The TUI is off in production
     * builds, so this whole block disappears.
     */
    if (tag && (
            !strcmp(tag, "HUNT")   ||
            !strcmp(tag, "SHD")    ||
            !strcmp(tag, "NID")    ||
            !strcmp(tag, "PERIOD") ||
            !strcmp(tag, "SLICE")  ||
            !strcmp(tag, "VFRM")   ||
            !strcmp(tag, "DSP")    ||
            !strcmp(tag, "OKDMP")  ||
            !strcmp(tag, "FLDMP")  ||
            !strcmp(tag, "IQR"))) {
        extern void sys_log(unsigned char color, const char *fmt, ...);
        sys_log(0, "%s %s", tag, body);
    }
#endif
}

void diag_line(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vline(tag, fmt, ap);
    va_end(ap);
}

/* ── NID dibit dump (now typed) ────────────────────────── */
void diag_dump_nid(const char *tag, const int *dibits33, int nac_raw,
                   const char *duid_raw, int ec, int verdict_ok,
                   const char *reason)
{
    if (!dibits33) return;

    /* Copy into a stack array of int because diag_field_t int_array
     * type uses int (matches the pipeline's existing int dibits).
     */
    int dibits[33];
    for (int i = 0; i < 33; i++) dibits[i] = dibits33[i] & 3;

    /* Compose the variant fields without and with `reason`. We always
     * have nac_raw, duid_raw, ec, verdict, dibits; reason is optional.
     */
    if (reason && *reason) {
        diag_emit(tag, 6, (diag_field_t[]){
            DF_HEX ("nac_raw",  nac_raw & 0xFFF),
            DF_STR ("duid_raw", duid_raw ? duid_raw : "??"),
            DF_INT ("ec",       ec),
            DF_BOOL("ok",       verdict_ok),
            DF_STR ("reason",   reason),
            DF_ARR ("dibits",   dibits, 33),
        });
    } else {
        diag_emit(tag, 5, (diag_field_t[]){
            DF_HEX ("nac_raw",  nac_raw & 0xFFF),
            DF_STR ("duid_raw", duid_raw ? duid_raw : "??"),
            DF_INT ("ec",       ec),
            DF_BOOL("ok",       verdict_ok),
            DF_ARR ("dibits",   dibits, 33),
        });
    }
}
