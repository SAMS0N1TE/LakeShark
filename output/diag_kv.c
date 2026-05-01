/*
 * diag_kv.c — kv text formatter on UART1 (GPIO 32 TX).
 *
 * Wire format:
 *   TAG ts=<float> [dateTime=<rfc3339>] [key=val ...]\r\n
 *
 * - TAG is left-padded to 5 chars so columns align in a terminal.
 * - ts is always present (uptime float seconds).
 * - dateTime is present only when diag_time has been synced.
 * - keys are emitted in producer order (no sorting).
 * - String values are emitted bare unless they contain whitespace,
 *   '=', or non-printable; in those cases they're quoted with
 *   embedded quotes/backslashes escaped.
 *
 * Producer→sink path is:
 *   diag_emit (caller)
 *     → diag_kv_record (this file: formats into a stack buffer)
 *     → xRingbufferSend (NOSPLIT, 4 KiB ring)
 *     → diag_kv_tx_task (pinned to core 1, drains ring → uart_write_bytes)
 *
 * On ring-full, we increment s_dropped and discard. The 1-Hz ERR
 * record in the periodic emit reports this.
 */
#include "diag_kv.h"
#include "diag.h"
#include "diag_time.h"

#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* Macros DIAG_UART_NUM / DIAG_UART_TX_PIN / DIAG_UART_RX_PIN /
 * DIAG_UART_BAUD / DIAG_LINE_MAX come from diag.h. The two values
 * that don't are kv-sink-specific:
 */
#ifndef DIAG_KV_RING_BYTES
#define DIAG_KV_RING_BYTES     4096
#endif

static const char *TAG_LOG = "DIAG_KV";

static RingbufHandle_t s_ring     = NULL;
static TaskHandle_t    s_task     = NULL;
static uint32_t        s_dropped  = 0;

/* ── drain task ────────────────────────────────────────────── */
static void diag_kv_tx_task(void *arg)
{
    (void)arg;
    while (1) {
        size_t len = 0;
        void *p = xRingbufferReceive(s_ring, &len, pdMS_TO_TICKS(200));
        if (p) {
            uart_write_bytes(DIAG_UART_NUM, (const char *)p, len);
            vRingbufferReturnItem(s_ring, p);
        }
    }
}

/* ── init ──────────────────────────────────────────────────── */
void diag_kv_init(void)
{
    if (s_ring) return;

    s_ring = xRingbufferCreate(DIAG_KV_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (!s_ring) {
        ESP_LOGE(TAG_LOG, "ringbuf alloc failed");
        return;
    }

    if (!uart_is_driver_installed(DIAG_UART_NUM)) {
        const uart_config_t cfg = {
            .baud_rate  = DIAG_UART_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        if (uart_driver_install(DIAG_UART_NUM, 1024, 4096, 0, NULL, 0) == ESP_OK) {
            uart_param_config(DIAG_UART_NUM, &cfg);
            uart_set_pin(DIAG_UART_NUM,
                         DIAG_UART_TX_PIN, DIAG_UART_RX_PIN,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

            xTaskCreatePinnedToCore(diag_kv_tx_task, "diag_kv_tx",
                                    3072, NULL, tskIDLE_PRIORITY + 2,
                                    &s_task, 1);
        }
    }
}

uint32_t diag_kv_dropped(void)
{
    uint32_t v = s_dropped;
    s_dropped = 0;
    return v;
}

/* ── string emission helpers ───────────────────────────────── */

/* True iff s would need quoting in kv output. */
static bool kv_needs_quote(const char *s)
{
    if (!s || !*s) return true;          /* empty string → quote it */
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '=' || *p == '"' || *p == '\\') return true;
        if (*p < 0x20 || *p == 0x7f) return true;
    }
    return false;
}

/* Append a string value to the line buffer at *off, with quoting if
 * needed. Bumps *off. Returns true on success, false on overflow.
 * On overflow, *off may be left mid-write; caller should bail.
 */
static bool kv_append_str(char *buf, int cap, int *off, const char *s)
{
    if (!s) s = "";
    bool quote = kv_needs_quote(s);
    if (quote) {
        if (*off + 1 >= cap) return false;
        buf[(*off)++] = '"';
        for (const char *p = s; *p; p++) {
            if (*p == '"' || *p == '\\') {
                if (*off + 2 >= cap) return false;
                buf[(*off)++] = '\\';
                buf[(*off)++] = *p;
            } else if (*off + 1 >= cap) {
                return false;
            } else {
                buf[(*off)++] = *p;
            }
        }
        if (*off + 1 >= cap) return false;
        buf[(*off)++] = '"';
    } else {
        int n = (int)strlen(s);
        if (*off + n >= cap) return false;
        memcpy(buf + *off, s, n);
        *off += n;
    }
    return true;
}

/* ── record formatter ──────────────────────────────────────── */
void diag_kv_record(const char *tag, int n_fields, const diag_field_t *fields)
{
    if (!s_ring) return;

    char buf[DIAG_LINE_MAX];
    int  off = 0;

    /* tag, left-padded to 5. */
    int n = snprintf(buf, sizeof(buf), "%-5s ts=%.3f",
                     tag ? tag : "?",
                     (double)diag_uptime_s());
    if (n < 0) return;
    off = n;

    /* dateTime if synced. */
    if (diag_time_synced()) {
        char dt[40];
        size_t dn = diag_time_format_rfc3339(dt, sizeof(dt));
        if (dn > 0) {
            int m = snprintf(buf + off, sizeof(buf) - off, " dateTime=%s", dt);
            if (m > 0 && off + m < (int)sizeof(buf)) off += m;
        }
    }

    /* fields */
    for (int i = 0; i < n_fields; i++) {
        const diag_field_t *f = &fields[i];
        if (!f->key) continue;

        if (off + 1 >= (int)sizeof(buf)) goto overflow;
        buf[off++] = ' ';

        int klen = (int)strlen(f->key);
        if (off + klen + 1 >= (int)sizeof(buf)) goto overflow;
        memcpy(buf + off, f->key, klen);
        off += klen;
        buf[off++] = '=';

        int m = 0;
        switch (f->type) {
        case DF_T_INT:
            m = snprintf(buf + off, sizeof(buf) - off, "%ld", f->v.i);
            break;
        case DF_T_UINT:
            m = snprintf(buf + off, sizeof(buf) - off, "%lu", f->v.u);
            break;
        case DF_T_FLOAT:
            m = snprintf(buf + off, sizeof(buf) - off, "%.6g", f->v.f);
            break;
        case DF_T_BOOL:
            m = snprintf(buf + off, sizeof(buf) - off, "%d", f->v.b ? 1 : 0);
            break;
        case DF_T_HEX:
            m = snprintf(buf + off, sizeof(buf) - off, "0x%lx", f->v.u);
            break;
        case DF_T_STR:
            if (!kv_append_str(buf, sizeof(buf) - 2, &off, f->v.s)) goto overflow;
            m = 0;
            break;
        case DF_T_INT_ARRAY:
            for (int j = 0; j < f->v.a.n; j++) {
                int q = snprintf(buf + off, sizeof(buf) - off,
                                 (j == 0) ? "%d" : ",%d", f->v.a.arr[j]);
                if (q < 0 || off + q >= (int)sizeof(buf) - 2) goto overflow;
                off += q;
            }
            m = 0;
            break;
        }
        if (m < 0) return;
        if (m > 0) {
            if (off + m >= (int)sizeof(buf) - 2) goto overflow;
            off += m;
        }
    }

    /* CRLF */
    if (off + 2 > (int)sizeof(buf)) off = (int)sizeof(buf) - 2;
    buf[off++] = '\r';
    buf[off++] = '\n';

    BaseType_t ok = xRingbufferSend(s_ring, buf, off, 0);
    if (ok != pdTRUE) s_dropped++;
    return;

overflow:
    /* Truncate cleanly: " ...\r\n" if there's room. */
    if (off + 6 <= (int)sizeof(buf)) {
        memcpy(buf + off, " ...\r\n", 6);
        off += 6;
    } else {
        off = (int)sizeof(buf) - 2;
        buf[off++] = '\r';
        buf[off++] = '\n';
    }
    BaseType_t ok2 = xRingbufferSend(s_ring, buf, off, 0);
    if (ok2 != pdTRUE) s_dropped++;
}
