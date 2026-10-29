/* SSTV glue for the FM receive task. See fm_sstv.h. */
#include "fm_sstv.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fm_bcast.h"
#include "ls_time.h"
#include "sstv.h"
#include "ls_textpack.h"

static const char *TAG = "sstv";

static sstv_t *s_dec;
static uint16_t *s_pix;               /* SSTV_MAX_W x 496, made once        */
static int s_w, s_h, s_rows;
static char s_mode[12];
static fm_sstv_status_t s_pub;
static int s_tick;
static bool s_was_on;
/* The file being written, a few rows a block, bottom row first (BMP). */
static FILE *s_file;
static int s_file_row;
static EXT_RAM_BSS_ATTR uint8_t s_line[SSTV_MAX_W * 3 + 4];

#define MAX_H 496

bool fm_sstv_wanted(void) { return fm_bcast_opt(FM_BC_SSTV); }

static void publish(void)
{
    sstv_status_t st;
    if (s_dec) sstv_status(s_dec, &st);
    else memset(&st, 0, sizeof(st));
    s_pub.on = fm_sstv_wanted();
    s_pub.receiving = st.phase == SSTV_IMAGE;
    snprintf(s_pub.mode, sizeof(s_pub.mode), "%s", s_mode);
    s_pub.w = s_w;
    s_pub.h = s_h;
    s_pub.rows = s_rows;
    s_pub.slant_ppm = st.slant_ppm;
    s_pub.tone_hz = st.tone_hz;
    s_pub.level = st.level;
    s_pub.syncs_found = st.syncs_found;
    s_pub.syncs_missed = st.syncs_missed;
    s_pub.images = st.images;
    s_pub.saving = s_file != NULL;
    fm_sstv_publish(&s_pub);
}

static void on_begin(void *u, const sstv_mode_t *m)
{
    (void)u;
    if (s_file) { fclose(s_file); s_file = NULL; }
    s_w = m->width;
    s_h = sstv_height(m);
    if (s_h > MAX_H) s_h = MAX_H;
    s_rows = 0;
    snprintf(s_mode, sizeof(s_mode), "%s", m->name);
    if (s_pix) memset(s_pix, 0, (size_t)SSTV_MAX_W * MAX_H * 2);
    fm_sstv_set_image(s_pix, s_w, s_h);
    ESP_LOGI(TAG, "VIS %u: %s %dx%d", m->vis, m->name, s_w, s_h);
    publish();
}

static void on_row(void *u, int y, const uint8_t *rgb, int w)
{
    (void)u;
    if (!s_pix || y < 0 || y >= s_h) return;
    uint16_t *row = s_pix + (size_t)y * s_w;
    for (int x = 0; x < w && x < s_w; ++x)
        row[x] = (uint16_t)(((rgb[3 * x] & 0xF8) << 8) | ((rgb[3 * x + 1] & 0xFC) << 3) | (rgb[3 * x + 2] >> 3));
    if (y + 1 > s_rows) s_rows = y + 1;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* A picture ends: open its file and write the header; rows follow, a few
   a block, from fm_sstv_audio. A part picture is kept too, named so. */
static void on_end(void *u, bool complete, int rows)
{
    (void)u;
    ESP_LOGI(TAG, "%s %s, %d rows", s_mode, complete ? "complete" : "cut short", rows);
    if (!s_pix || rows < 16) { publish(); return; }
    mkdir("/sdcard/sstv", 0775);
    char base[40], path[96];
    ls_time_render_filename(base, sizeof(base), "SSTV");
    char tag[12];
    snprintf(tag, sizeof(tag), "%s", s_mode);
    for (char *c = tag; *c; ++c) if (*c == ' ') *c = '_';
    LS_SNPRINTF(t_fm_sstv_sdcard_sstv_s_s, path, sizeof(path), "/sdcard/sstv/%s_%s%s.bmp", base, tag, complete ? "" : "_part");
    s_file = fopen(path, "wb");
    if (!s_file) { ESP_LOGW(TAG, "no card for %s (%d)", path, errno); publish(); return; }
    const int h = s_rows, stride = (s_w * 3 + 3) & ~3;
    uint8_t hd[54] = { 'B', 'M' };
    put32(hd + 2, 54u + (uint32_t)stride * h);
    put32(hd + 10, 54);
    put32(hd + 14, 40);
    put32(hd + 18, (uint32_t)s_w);
    put32(hd + 22, (uint32_t)h);
    hd[26] = 1; hd[28] = 24;
    put32(hd + 34, (uint32_t)stride * h);
    fwrite(hd, 1, sizeof(hd), s_file);
    s_file_row = h - 1;
    snprintf(s_pub.saved, sizeof(s_pub.saved), "%s", path + 8);
    publish();
}

static void write_some(int n)
{
    const int stride = (s_w * 3 + 3) & ~3;
    while (s_file && n-- > 0) {
        if (s_file_row < 0) {
            fclose(s_file);
            s_file = NULL;
            ESP_LOGI(TAG, "saved /sdcard/%s", s_pub.saved);
            publish();
            return;
        }
        const uint16_t *row = s_pix + (size_t)s_file_row * s_w;
        memset(s_line, 0, (size_t)stride);
        for (int x = 0; x < s_w; ++x) {
            const uint16_t p = row[x];
            s_line[3 * x]     = (uint8_t)((p & 0x1F) << 3);
            s_line[3 * x + 1] = (uint8_t)(((p >> 5) & 0x3F) << 2);
            s_line[3 * x + 2] = (uint8_t)((p >> 11) << 3);
        }
        if (fwrite(s_line, 1, (size_t)stride, s_file) != (size_t)stride) {
            fclose(s_file);
            s_file = NULL;
            ESP_LOGW(TAG, "card write failed");
            return;
        }
        s_file_row--;
    }
}

static bool ensure(void)
{
    if (s_dec && s_pix) return true;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (!s_dec) {
        s_dec = heap_caps_calloc(1, sizeof(*s_dec), caps);
        if (s_dec) {
            const sstv_sink_t sink = { on_begin, on_row, on_end, NULL };
            sstv_init(s_dec, &sink);
        }
    }
    if (!s_pix) s_pix = heap_caps_calloc((size_t)SSTV_MAX_W * MAX_H, 2, caps);
    if (s_dec && s_pix) return true;
    ESP_LOGE(TAG, "no PSRAM for SSTV");
    return false;
}

void fm_sstv_reset(void)
{
    if (s_dec) sstv_reset(s_dec);
}

void fm_sstv_service(void)
{
    if (s_file) write_some(16);
}

void fm_sstv_audio(const int16_t *pcm, int n)
{
    const bool on = fm_sstv_wanted();
    if (!on) {
        if (s_was_on) { s_was_on = false; fm_sstv_reset(); publish(); }
        write_some(8);
        return;
    }
    if (!ensure()) return;
    s_was_on = true;
    if (pcm) sstv_push(s_dec, pcm, n);
    else {
        static const int16_t quiet[64];
        while (n > 0) {
            const int k = n > 64 ? 64 : n;
            sstv_push(s_dec, quiet, k);
            n -= k;
        }
    }
    write_some(8);
    if (++s_tick >= 4) { s_tick = 0; publish(); }
}
