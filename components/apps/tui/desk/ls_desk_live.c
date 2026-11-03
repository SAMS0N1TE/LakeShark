/* Each radio app's live line for its taskbar button and Start menu row,
   read from the same state its screen draws: FM's frequency, P25's
   talkgroup (or where it is tuned), ADS-B's aircraft count. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <string.h>

#include "apps/fm/fm_state.h"
#include "apps/p25/p25_state.h"
#include "apps/adsb/adsb_state.h"

const char *dk_live_line(const ls_app_t *a, char *buf, size_t n)
{
    if (!a || !a->id || !buf || n < 8) return NULL;
    if (!strcmp(a->id, "fm") && FM.freq_hz) {
        snprintf(buf, n, "%lu.%03lu", (unsigned long)(FM.freq_hz / 1000000u),
                 (unsigned long)(FM.freq_hz / 1000u % 1000u));
        return buf;
    }
    if (!strcmp(a->id, "p25")) {
        const unsigned tg = P25.grant_talkgroup ? P25.grant_talkgroup : P25.p25_lcw_talkgroup;
        if (tg) { snprintf(buf, n, "TG %u", tg); return buf; }
        if (s_tune_freq_hz) {
            snprintf(buf, n, "%lu.%04lu", (unsigned long)(s_tune_freq_hz / 1000000u),
                     (unsigned long)(s_tune_freq_hz / 100u % 10000u));
            return buf;
        }
        return NULL;
    }
    if (!strcmp(a->id, "adsb")) {
        snprintf(buf, n, "%d ac", adsb_state_active_count());
        return buf;
    }
    return NULL;
}

/* ------------------------------------------------- the board around it -- */

#include <dirent.h>
#include <strings.h>
#ifdef ESP_PLATFORM
#include "ls_board.h"
#endif
#include <stdlib.h>
#include "settings.h"
#include "../ls_theme.h"

extern int  audio_volume_get(void);
extern void audio_volume_set(int v);
extern bool audio_is_muted(void);
extern void audio_mute_set(bool muted);
extern int  ls_time_source(void);

int dk_sys_volume(void)
{
    const int v = audio_volume_get();
    return v < 0 ? 0 : v > 100 ? 100 : v;
}
void dk_sys_set_volume(int pct)
{
    audio_volume_set(pct < 0 ? 0 : pct > 100 ? 100 : pct);
}
bool dk_sys_muted(void) { return audio_is_muted(); }
void dk_sys_set_mute(bool on) { audio_mute_set(on); }
int dk_sys_time_source(void) { return ls_time_source(); }

#if defined(ESP_PLATFORM) && defined(CONFIG_LS_BLE_HEAD) && CONFIG_LS_BLE_HEAD
#include "../../../../main/ble_hid.h"
#pragma weak hid_list_bonds
#pragma weak hid_list_devices
int dk_sys_bt(int *connected)
{
    if (connected) *connected = 0;
    if (!hid_list_bonds || !hid_list_devices) return -1;
    hid_device_t d[HID_BONDS];
    uint32_t ver = 0;
    const int bonded = (int)hid_list_bonds(d, HID_BONDS, &ver);
    int up = 0;
    for (int i = 0; i < bonded; i++) up += d[i].state == HID_READY;
    if (connected) *connected = up;
    return bonded;
}
#else
int dk_sys_bt(int *connected)
{
    if (connected) *connected = 0;
    return -1;                                   /* no Bluetooth host in this build */
}
#endif

int dk_sys_saved_theme(void) { return settings_get_desk_theme(); }
void dk_sys_save_theme(int theme) { settings_set_desk_theme(theme); }
uint32_t dk_sys_saved_wallpaper(void) { return settings_get_desk_wallpaper(); }
void dk_sys_save_wallpaper(uint32_t id) { settings_set_desk_wallpaper(id); }
uint32_t dk_sys_saved_word(int slot) { return settings_get_desk_word(slot); }
void dk_sys_save_word(int slot, uint32_t v) { settings_set_desk_word(slot, v); }

static const char *wall_dir(void)
{
#ifdef ESP_PLATFORM
    return "/sdcard/desk/wallpapers";
#else
    const char *d = getenv("LSSIM_DESK_WALLDIR");
    return d ? d : "desk-wallpapers";
#endif
}

static bool picture_name(const char *n)
{
    const size_t l = strlen(n);
    if (l < 5 || l >= 40 || n[0] == '.') return false;
    const char *e = n + l - 4;
    return !strcasecmp(e, ".bmp") || !strcasecmp(e, ".png") || !strcasecmp(e, ".jpg");
}

int dk_sys_wallpaper_files(char names[][40], int cap)
{
    DIR *d = opendir(wall_dir());
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < cap)
        if (picture_name(e->d_name)) snprintf(names[n++], 40, "%s", e->d_name);
    closedir(d);
    for (int a = 1; a < n; a++)                                  /* sorted, so the list is stable */
        for (int b = a; b > 0 && strcmp(names[b], names[b - 1]) < 0; b--) {
            char t[40];
            memcpy(t, names[b], 40); memcpy(names[b], names[b - 1], 40); memcpy(names[b - 1], t, 40);
        }
    return n;
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 8L * 1024 * 1024) { fclose(f); return NULL; }
    uint8_t *b = dk_psram((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { dk_psram_free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Cover (out_w x out_h) from an RGB source by nearest sample, centred, the
   way a desktop fills: the picture's short side fits, the rest is cropped. */
typedef void (*rgb_at_fn)(const void *ctx, int x, int y, uint8_t *r, uint8_t *g, uint8_t *b);
static void cover(uint16_t *out, int ow, int oh, int sw, int sh, rgb_at_fn at, const void *ctx)
{
    /* scale = max(ow/sw, oh/sh) in 16.16 */
    const int64_t kx = ((int64_t)ow << 16) / sw, ky = ((int64_t)oh << 16) / sh;
    const int64_t k = kx > ky ? kx : ky;
    const int vw = (int)(((int64_t)sw * k) >> 16), vh = (int)(((int64_t)sh * k) >> 16);
    const int ox = (vw - ow) / 2, oy = (vh - oh) / 2;
    for (int y = 0; y < oh; y++) {
        int sy = (int)((((int64_t)(y + oy)) << 16) / k);
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < ow; x++) {
            int sx = (int)((((int64_t)(x + ox)) << 16) / k);
            if (sx >= sw) sx = sw - 1;
            uint8_t r, g, b;
            at(ctx, sx, sy, &r, &g, &b);
            out[(size_t)y * ow + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    }
}

typedef struct { const uint8_t *px; int w, h, bpp; size_t stride; bool bottom_up; } bmp_t;
static void bmp_at(const void *ctx, int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    const bmp_t *m = ctx;
    const int row = m->bottom_up ? m->h - 1 - y : y;
    const uint8_t *p = m->px + (size_t)row * m->stride + (size_t)x * (m->bpp / 8);
    *b = p[0]; *g = p[1]; *r = p[2];
}

#if defined(ESP_PLATFORM) && LS_HAS_CARTOCORE
#include "cartocore/core.h"
#include "cartocore/media.h"
static void frame_at(const void *ctx, int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    const cc_rgb_frame *f = ctx;
    const uint8_t *p = f->pixels + (size_t)y * f->stride + (size_t)x * 3;
    *r = p[0]; *g = p[1]; *b = p[2];
}
#endif

/* The theme's colour for a TUI index, as RGB565, for the ground round a picture. */
static uint16_t ground565(uint8_t idx)
{
    const ls_tui_theme_t *t = ls_tui_theme_effective(ls_tui_get_theme(), ls_tui_daylight());
    return t->palette[idx & 15];
}

/* Fill, tile, centre or stretch a source into out (ow x oh); `scale` is
   output pixels per source pixel in 16.16 for tile and centre. */
static void place(uint16_t *out, int ow, int oh, int sw, int sh, int fit, int64_t scale, uint16_t g,
                  rgb_at_fn at, const void *ctx)
{
    if (fit == DK_FIT_FILL) { cover(out, ow, oh, sw, sh, at, ctx); return; }
    const int vw = fit == DK_FIT_STRETCH ? ow : (int)(((int64_t)sw * scale) >> 16);
    const int vh = fit == DK_FIT_STRETCH ? oh : (int)(((int64_t)sh * scale) >> 16);
    const int ox = fit == DK_FIT_CENTRE ? (ow - vw) / 2 : 0, oy = fit == DK_FIT_CENTRE ? (oh - vh) / 2 : 0;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int u = x - ox, v = y - oy;
            if (fit == DK_FIT_TILE) { u %= vw > 0 ? vw : 1; v %= vh > 0 ? vh : 1; }
            if (u < 0 || v < 0 || u >= vw || v >= vh || vw <= 0 || vh <= 0) { out[(size_t)y * ow + x] = g; continue; }
            int sx = (int)((int64_t)u * sw / vw), sy = (int)((int64_t)v * sh / vh);
            if (sx >= sw) sx = sw - 1;
            if (sy >= sh) sy = sh - 1;
            uint8_t r, gg, b;
            at(ctx, sx, sy, &r, &gg, &b);
            out[(size_t)y * ow + x] = (uint16_t)((r >> 3) << 11 | (gg >> 2) << 5 | (b >> 3));
        }
}

bool dk_sys_load_wallpaper(const char *name, int ow, int oh, int fit, int full_w, uint8_t gidx,
                           uint16_t **px, int *w, int *h)
{
    const int64_t scale = full_w > 0 ? ((int64_t)ow << 16) / full_w : 1 << 16;
    const uint16_t g = ground565(gidx);
    if (!name || ow < 8 || oh < 8) return false;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", wall_dir(), name);
    size_t len = 0;
    uint8_t *data = read_all(path, &len);
    if (!data) return false;
    uint16_t *out = dk_psram((size_t)ow * oh * sizeof(uint16_t));
    bool ok = false;
    if (out && len > 54 && data[0] == 'B' && data[1] == 'M') {
        const uint32_t off = le32(data + 10);
        const int32_t bw = (int32_t)le32(data + 18), bh = (int32_t)le32(data + 22);
        const int bpp = data[28] | data[29] << 8;
        const uint32_t comp = le32(data + 30);
        if ((bpp == 24 || bpp == 32) && (comp == 0 || comp == 3) && bw > 0 && bh != 0) {
            bmp_t m = {data + off, bw, bh < 0 ? -bh : bh, bpp, ((size_t)bw * (bpp / 8) + 3) & ~(size_t)3, bh > 0};
            if (off + m.stride * (size_t)m.h <= len) {
                place(out, ow, oh, m.w, m.h, fit, scale, g, bmp_at, &m);
                ok = true;
            }
        }
    }
#if defined(ESP_PLATFORM) && LS_HAS_CARTOCORE
    else if (out) {
        /* PNG and JPEG through CartoCore's decoder, in a PSRAM arena. */
        const size_t arena_bytes = 12u * 1024 * 1024;
        void *arena_mem = dk_psram(arena_bytes);
        if (arena_mem) {
            cc_arena a;
            cc_arena_init(&a, arena_mem, arena_bytes);
            cc_rgb_frame f;
            if (cc_media_decode(data, len, &a, &f) == 0 && f.width && f.height) {
                place(out, ow, oh, (int)f.width, (int)f.height, fit, scale, g, frame_at, &f);
                ok = true;
            }
            dk_psram_free(arena_mem);
        }
    }
#endif
    dk_psram_free(data);
    if (!ok) { dk_psram_free(out); return false; }
    *px = out;
    *w = ow;
    *h = oh;
    return true;
}

#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
const char *ls_desk_version(void) { return esp_app_get_description()->version; }
#else
const char *ls_desk_version(void) { return "lssim"; }
#endif
