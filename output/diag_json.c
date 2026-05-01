/*
 * diag_json.c — JSONL formatter on UART2 GPIO 3.
 *
 * Wire format:
 *   \x1e{"ts":<float>,"tag":"<TAG>"[,"dateTime":"..."][,<fields>]}\n
 *
 * Each record is one self-contained JSON object preceded by ASCII RS
 * (0x1e) and terminated with LF. A host connecting mid-stream scans
 * forward to the next 0x1e to align.
 *
 * Output: UART2, TX = GPIO 3, 115200 8N1, TX-only (no RX wire).
 * This is the pin previously used by the (deleted) ADS-B JSONL output
 * — repurposed for P25 telemetry while we debug the front end.
 *
 * Producer→sink path: diag_emit → diag_json_record → ringbuf →
 * diag_json_tx_task → uart_write_bytes. Drain task is pinned to
 * core 1 so it never fights core 0's USB host loop.
 *
 * No RX path on this build. Without inbound commands, the dateTime
 * field in records stays absent (only `ts` uptime is emitted). If
 * wall-clock timestamping ever matters, wire RX to a free GPIO and
 * restore the parser from git history.
 */
#include "diag_json.h"
#include "diag_time.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>

#ifndef DIAG_JSON_UART_NUM
#define DIAG_JSON_UART_NUM     2
#endif
#ifndef DIAG_JSON_TX_PIN
#define DIAG_JSON_TX_PIN       3
#endif
#ifndef DIAG_JSON_BAUD
#define DIAG_JSON_BAUD         115200
#endif
#ifndef DIAG_JSON_RING_BYTES
#define DIAG_JSON_RING_BYTES   8192
#endif
#ifndef DIAG_JSON_LINE_MAX
#define DIAG_JSON_LINE_MAX     1024
#endif

static const char *TAG_LOG = "DIAG_JSON";

static RingbufHandle_t s_ring     = NULL;
static TaskHandle_t    s_tx_task  = NULL;
static uint32_t        s_dropped  = 0;

/* ── drain task ──────────────────────────────────────────── */
static void diag_json_tx_task(void *arg)
{
    (void)arg;
    while (1) {
        size_t len = 0;
        void *p = xRingbufferReceive(s_ring, &len, pdMS_TO_TICKS(200));
        if (p) {
            /* uart_write_bytes copies into the IDF TX queue (4 KiB
             * configured at install). Pinned to core 1, so even
             * if the TX queue fills momentarily, core 0's USB host
             * loop never blocks. */
            uart_write_bytes(DIAG_JSON_UART_NUM, (const char *)p, len);
            vRingbufferReturnItem(s_ring, p);
        }
    }
}

/* ── init ──────────────────────────────────────────────── */
void diag_json_init(void)
{
    if (s_ring) return;

    s_ring = xRingbufferCreate(DIAG_JSON_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (!s_ring) {
        ESP_LOGE(TAG_LOG, "ringbuf alloc failed");
        return;
    }

    /* UART2 driver install. TX queue 4 KiB; we drain via our own
     * ringbuf above so the TX queue exists only to absorb burst
     * writes from uart_write_bytes. RX queue 0 — we don't read.
     * Pin RX to UART_PIN_NO_CHANGE because we have no inbound wire.
     */
    if (!uart_is_driver_installed(DIAG_JSON_UART_NUM)) {
        const uart_config_t cfg = {
            .baud_rate  = DIAG_JSON_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        if (uart_driver_install(DIAG_JSON_UART_NUM, 256, 4096, 0, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG_LOG, "uart driver install failed");
            return;
        }
        uart_param_config(DIAG_JSON_UART_NUM, &cfg);
        uart_set_pin(DIAG_JSON_UART_NUM,
                     DIAG_JSON_TX_PIN, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    xTaskCreatePinnedToCore(diag_json_tx_task, "diag_jsn_tx",
                            3072, NULL, tskIDLE_PRIORITY + 2,
                            &s_tx_task, 1);
}

uint32_t diag_json_dropped(void)
{
    uint32_t v = s_dropped;
    s_dropped = 0;
    return v;
}

/* ── string emission helpers ──────────────────────────── */

/* JSON-escape a string into buf at *off. Returns true on success. */
static bool json_emit_string(char *buf, int cap, int *off, const char *s)
{
    if (*off + 1 >= cap) return false;
    buf[(*off)++] = '"';
    if (!s) s = "";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (*off + 2 >= cap) return false;
            buf[(*off)++] = '\\';
            buf[(*off)++] = (char)*p;
        } else if (*p == '\n') {
            if (*off + 2 >= cap) return false;
            buf[(*off)++] = '\\'; buf[(*off)++] = 'n';
        } else if (*p == '\r') {
            if (*off + 2 >= cap) return false;
            buf[(*off)++] = '\\'; buf[(*off)++] = 'r';
        } else if (*p == '\t') {
            if (*off + 2 >= cap) return false;
            buf[(*off)++] = '\\'; buf[(*off)++] = 't';
        } else if (*p < 0x20) {
            if (*off + 6 >= cap) return false;
            int m = snprintf(buf + *off, cap - *off, "\\u%04x", (unsigned)*p);
            if (m < 0) return false;
            *off += m;
        } else {
            if (*off + 1 >= cap) return false;
            buf[(*off)++] = (char)*p;
        }
    }
    if (*off + 1 >= cap) return false;
    buf[(*off)++] = '"';
    return true;
}

/* ── record formatter ──────────────────────────────────── */
void diag_json_record(const char *tag, int n_fields, const diag_field_t *fields)
{
    if (!s_ring) return;

    char buf[DIAG_JSON_LINE_MAX];
    int  off = 0;

    /* Leading record separator. */
    if (off + 1 >= (int)sizeof(buf)) return;
    buf[off++] = 0x1e;

    /* Open object, ts, tag. */
    int n = snprintf(buf + off, sizeof(buf) - off,
                     "{\"ts\":%.3f,\"tag\":",
                     (double)diag_uptime_s());
    if (n < 0) return;
    off += n;
    if (!json_emit_string(buf, sizeof(buf) - 4, &off, tag ? tag : "?")) goto overflow;

    /* dateTime if synced. */
    if (diag_time_synced()) {
        char dt[40];
        size_t dn = diag_time_format_rfc3339(dt, sizeof(dt));
        if (dn > 0) {
            int m = snprintf(buf + off, sizeof(buf) - off, ",\"dateTime\":");
            if (m < 0) goto overflow;
            off += m;
            if (!json_emit_string(buf, sizeof(buf) - 4, &off, dt)) goto overflow;
        }
    }

    /* Fields. */
    for (int i = 0; i < n_fields; i++) {
        const diag_field_t *f = &fields[i];
        if (!f->key) continue;

        if (off + 2 >= (int)sizeof(buf)) goto overflow;
        buf[off++] = ',';
        if (!json_emit_string(buf, sizeof(buf) - 4, &off, f->key)) goto overflow;
        if (off + 1 >= (int)sizeof(buf)) goto overflow;
        buf[off++] = ':';

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
            m = snprintf(buf + off, sizeof(buf) - off, f->v.b ? "true" : "false");
            break;
        case DF_T_HEX:
            /* JSON has no hex literal; emit as decimal. The kv form
             * preserves the 0x prefix. JSON consumers do `value &
             * mask` math, decimal is fine.
             */
            m = snprintf(buf + off, sizeof(buf) - off, "%lu", f->v.u);
            break;
        case DF_T_STR:
            if (!json_emit_string(buf, sizeof(buf) - 4, &off, f->v.s)) goto overflow;
            m = 0;
            break;
        case DF_T_INT_ARRAY:
            if (off + 1 >= (int)sizeof(buf)) goto overflow;
            buf[off++] = '[';
            for (int j = 0; j < f->v.a.n; j++) {
                int q = snprintf(buf + off, sizeof(buf) - off,
                                 (j == 0) ? "%d" : ",%d", f->v.a.arr[j]);
                if (q < 0 || off + q >= (int)sizeof(buf) - 4) goto overflow;
                off += q;
            }
            if (off + 1 >= (int)sizeof(buf)) goto overflow;
            buf[off++] = ']';
            m = 0;
            break;
        }
        if (m < 0) return;
        if (m > 0) {
            if (off + m >= (int)sizeof(buf) - 4) goto overflow;
            off += m;
        }
    }

    /* Close object, terminator. */
    if (off + 2 > (int)sizeof(buf)) goto overflow;
    buf[off++] = '}';
    buf[off++] = '\n';

    BaseType_t ok = xRingbufferSend(s_ring, buf, off, 0);
    if (ok != pdTRUE) s_dropped++;
    return;

overflow:
    /* On overflow, emit an ERR record marking the truncation. The
     * partially-built record gets dropped — sending malformed JSON
     * would be worse than dropping. We don't recurse into diag_emit
     * here because of the ringbuf state.
     */
    s_dropped++;
}
