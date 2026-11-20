/* ls_report: the redacted bug-report bundle and its upload. See core/ls_hub.h. */
#include "ls_hub.h"
#include "ls_hub_int.h"
#include "ls_bootlog.h"
#include "ls_crash.h"
#include "ls_errlog.h"
#include "ls_location.h"
#include "ls_redact.h"
#include "ls_safe_mode.h"
#include "ls_version.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OUT_CAP   (96 * 1024)
#define SCR_CAP   (20 * 1024)
#define BOOT_HEAD (6 * 1024)
#define BOOT_TAIL (10 * 1024)

typedef struct { char *o; size_t n; char *scr; bool gps; } bundle_t;

static void raw(bundle_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void raw(bundle_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(b->o + b->n, OUT_CAP - b->n, fmt, ap);
    va_end(ap);
    if (k > 0) b->n += (size_t)k < OUT_CAP - b->n ? (size_t)k : OUT_CAP - b->n - 1;
}

/* "key":"<text redacted and escaped>", */
static void text(bundle_t *b, const char *key, const char *src, size_t n)
{
    if (n >= SCR_CAP) { src += n - (SCR_CAP - 1); n = SCR_CAP - 1; }
    memcpy(b->scr, src, n);
    b->scr[n] = 0;
    n = ls_redact(b->scr, b->gps);
    raw(b, "\"%s\":\"", key);
    b->n = ls_json_esc(b->o, b->n, OUT_CAP - 4, b->scr, n);
    raw(b, "\",");
}

static bool build(bundle_t *b, const char *note)
{
    ls_version_info_t v;
    char line[LS_VERSION_LINE_MAX];
    ls_version_get(&v);
    ls_version_line(line, sizeof(line));
    b->n = 0;
    raw(b, "{\"v\":1,\"board\":\"%s\",\"version\":\"%s\",\"uptime_s\":%" PRIu64 ",\"tasks\":%u,", v.board, v.version,
        (uint64_t)(esp_timer_get_time() / 1000000), (unsigned)uxTaskGetNumberOfTasks());
    raw(b, "\"heap\":{\"int_free\":%u,\"int_min\":%u,\"int_big\":%u,\"psram_free\":%u,\"psram_min\":%u,\"dma_free\":%u},",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    if (note && *note) text(b, "note", note, strlen(note));
    text(b, "build", line, strlen(line));
    char *t = heap_caps_malloc(SCR_CAP, MALLOC_CAP_SPIRAM);
    if (!t) return false;
    size_t k = ls_safe_report(t, SCR_CAP);
    text(b, "reset", t, k);
    k = ls_crash_format(t, SCR_CAP);
    text(b, "crash", t, k);
    raw(b, "\"errlog\":[");
    for (int i = 0; i < ls_errlog_count(); i++) {
        ls_errlog_rec_t r;
        if (!ls_errlog_get(i, &r)) break;
        k = ls_errlog_format(&r, t, SCR_CAP);
        raw(b, "%s\"", i ? "," : "");
        if (k >= SCR_CAP) k = SCR_CAP - 1;
        memcpy(b->scr, t, k);
        b->scr[k] = 0;
        k = ls_redact(b->scr, b->gps);
        b->n = ls_json_esc(b->o, b->n, OUT_CAP - 4, b->scr, k);
        raw(b, "\"");
    }
    raw(b, "],");
    heap_caps_free(t);
    char *bl = heap_caps_malloc(LS_BOOTLOG_TEXT_MAX, MALLOC_CAP_SPIRAM);
    if (bl) {
        k = ls_bootlog_text(bl, LS_BOOTLOG_TEXT_MAX);
        if (k > BOOT_HEAD + BOOT_TAIL) {
            memmove(bl + BOOT_HEAD + 7, bl + k - BOOT_TAIL, BOOT_TAIL);
            memcpy(bl + BOOT_HEAD, "\n[...]\n", 7);
            k = BOOT_HEAD + 7 + BOOT_TAIL;
        }
        text(b, "boot", bl, k);
        heap_caps_free(bl);
    }
    if (b->gps) {
        double lat, lon;
        if (ls_location_here(&lat, &lon) != LS_LOC_SRC_NONE) raw(b, "\"gps\":{\"lat\":%.2f,\"lon\":%.2f},", lat, lon);
    }
    raw(b, "\"end\":1}");
    return true;
}

size_t ls_report_preview(char *out, size_t cap, bool with_gps)
{
    bundle_t b = { .o = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM), .scr = heap_caps_malloc(SCR_CAP, MALLOC_CAP_SPIRAM), .gps = with_gps };
    size_t n = 0;
    if (b.o && b.scr && build(&b, "")) {
        n = b.n < cap - 1 ? b.n : cap - 1;
        memcpy(out, b.o, n);
    }
    if (cap) out[n] = 0;
    heap_caps_free(b.o);
    heap_caps_free(b.scr);
    return n;
}

void report_run(const char *note, bool gps)
{
    bundle_t b = { .o = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM), .scr = heap_caps_malloc(SCR_CAP, MALLOC_CAP_SPIRAM), .gps = gps };
    char rep[200] = "";
    if (!b.o || !b.scr || !build(&b, note)) { hub_set(LS_HUB_FAIL, "report", "out of memory"); goto out; }
    hub_req_t q = { .method = "POST", .path = "/v1/report", .ctype = "application/json", .body = b.o, .blen = (uint32_t)b.n, .out = rep, .ocap = sizeof(rep) };
    const int s = hub_call(&q, NULL);
    if (s == 200) {
        char id[24] = "";
        hub_json_str(rep, "id", id, sizeof(id));
        const ls_errlog_rec_t *e = ls_errlog_newest();
        if (e) hub_rep_seq_set(e->seq);
        hub_set(LS_HUB_OK, "report", "sent %s", id);
    } else if (s == 429) hub_set(LS_HUB_FAIL, "report", "too many reports, try later");
    else if (s == -1) hub_set(LS_HUB_FAIL, "report", "no Wi-Fi");
    else hub_set(LS_HUB_FAIL, "report", "failed (%d)", s);
out:
    heap_caps_free(b.o);
    heap_caps_free(b.scr);
}

bool ls_report_send(const char *note, bool with_gps) { return hub_submit(HOP_REPORT, note ? note : "", with_gps); }

bool ls_report_offer_pending(void)
{
    const ls_errlog_rec_t *e = ls_errlog_newest();
    return e && e->seq != hub_rep_seq();
}

void ls_report_offer_done(void)
{
    const ls_errlog_rec_t *e = ls_errlog_newest();
    if (e) hub_rep_seq_set(e->seq);
}

void report_console(int argc, char **argv)
{
    const char *v = argc > 1 ? argv[1] : "";
    if (!strcmp(v, "send")) {
        char note[100] = "";
        for (int i = 2; i < argc; i++) { strlcat(note, note[0] ? " " : "", sizeof(note)); strlcat(note, argv[i], sizeof(note)); }
        ls_report_send(note, false);
        hub_wait(60000);
        ls_hub_status_t st;
        ls_hub_status(&st);
        printf("report: %s\n", st.text);
    } else if (!strcmp(v, "preview")) {
        char *o = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM);
        if (!o) { printf("report: out of memory\n"); return; }
        const size_t n = ls_report_preview(o, OUT_CAP, argc > 2 && !strcmp(argv[2], "gps"));
        fwrite(o, 1, n, stdout);
        printf("\nreport: %u bytes would be sent\n", (unsigned)n);
        heap_caps_free(o);
    } else if (!strcmp(v, "id")) {
        char id[17];
        printf("report: device id %s, crash offer %s\n", ls_hub_device_id(id) ? id : "?", ls_report_offer_pending() ? "pending" : "none");
    } else {
        printf("report send [note] | preview [gps] | id\n");
    }
}
