/* See ls_lap_api.h. API calls have no effect outside a host callback. */
#include "ls_lap_api.h"
#include "../../../app-sdk/include/lap3d.h"
#include "ls_glyph.h"
#include "ls_lap_storage.h"
#include "ls_value.h"
#include "apps/adsb/adsb_state.h"
#include "apps/adsb/adsb_demo.h"
#include "esp_timer.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#else
#define MAKE_DIR(p) mkdir(p, 0777)
#endif
#ifdef ESP_PLATFORM
#include "audio/tone.h"
#include "core/settings.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define DEALLOC(p) heap_caps_free(p)
#define RESIZE(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define EXT_RAM_BSS_ATTR
#define ALLOC(n) malloc(n)
#define DEALLOC(p) free(p)
#define RESIZE(p, n) realloc((p), (n))
#endif
typedef union block block;
union block {
    struct {
        block *next;
        size_t size;
        uint32_t gen;
    } h;
    max_align_t alignment;
};
typedef struct cache cache;
struct cache {
    cache *next;
    char name[49];
    unsigned char *data;
    size_t size, capacity;
    bool dirty, removed;
};
typedef struct {
    cache *file;
    size_t pos;
    char mode;
} handle;
EXT_RAM_BSS_ATTR static struct {
    char id[32], error[96], missing[96], data_root[41];
    uint32_t generation, frame, started;
    bool callback, exit;
    tui_surface *surface;
    tui_rect area;
    bool image_called, image_active;
    tui_rect image_area;
    const uint16_t *image_pixels;
    int image_w, image_h;
    uint32_t image_serial;
    block *blocks;
    uintptr_t image_base;
    size_t image_bytes;
    char log[8][160];
    atomic_uint log_head, log_tail;
    cache *files;
    handle fd[4];
#ifdef ESP_PLATFORM
    TaskHandle_t owner;
#endif
} s;
EXT_RAM_BSS_ATTR static atomic_size_t bytes_used;
EXT_RAM_BSS_ATTR static atomic_uint bad_frees;
static void (*wake_io)(void);
void ls_lap_api_set_wake(void (*wake)(void)) { wake_io = wake; }
#ifndef ESP_PLATFORM
uint32_t (*ls_lap_api_host_clock)(void); /* the bench's clock, when it has one */
#endif
static uint32_t now(void) {
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000);
#else
    return ls_lap_api_host_clock ? ls_lap_api_host_clock() : 0;
#endif
}
static bool inside(void) {
#ifdef ESP_PLATFORM
    return s.callback && xTaskGetCurrentTaskHandle() == s.owner;
#else
    return s.callback;
#endif
}
bool ls_lap_api_inside(void) { return inside(); }
void ls_lap_api_set_image(const void *base, size_t bytes) {
    s.image_base = (uintptr_t)base;
    s.image_bytes = bytes;
}
/* p..p+n lies wholly in one live allocation of this app, or in its loaded
   image (static buffers): somewhere a card job may copy into. */
bool ls_lap_api_owns(const void *p, size_t n) {
    const uintptr_t a = (uintptr_t)p;
    if (!p || a + n < a)
        return false;
    if (s.image_bytes && a >= s.image_base && a + n <= s.image_base + s.image_bytes)
        return true;
    for (block *b = s.blocks; b; b = b->h.next) {
        const uintptr_t lo = (uintptr_t)(b + 1);
        if (b->h.gen == s.generation && a >= lo && a + n <= lo + b->h.size)
            return true;
    }
    return false;
}
static void error(const char *text) { snprintf(s.error, sizeof(s.error), "%s", text); }
void ls_lap_api_begin(const char *id, uint32_t gen, uint32_t opened_ms) {
    const uintptr_t image_base = s.image_base;
    const size_t image_bytes = s.image_bytes;
    memset(&s, 0, sizeof(s));
    s.image_base = image_base;
    s.image_bytes = image_bytes;
    snprintf(s.id, sizeof(s.id), "%s", id);
    s.generation = gen;
    s.started = opened_ms;
    atomic_store(&bytes_used, 0);
    atomic_store(&bad_frees, 0);
    atomic_init(&s.log_head, 0);
    atomic_init(&s.log_tail, 0);
}
void ls_lap_api_callback(bool on) {
#ifdef ESP_PLATFORM
    s.owner = on ? xTaskGetCurrentTaskHandle() : NULL;
#endif
    s.callback = on;
}
void ls_lap_api_draw_begin(tui_surface *sf, tui_rect area) {
    s.surface = sf;
    s.area = tui_rect_intersect(area, tui_surface_rect(sf));
    s.frame++;
    s.image_called = false;
}
void ls_lap_api_draw_end(void) {
    if (s.surface && s.image_active && !s.image_called) {
        ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
        s.image_active = false;
    }
    s.surface = NULL;
}
bool ls_lap_api_exit_requested(void) { return s.exit; }
size_t ls_lap_api_bytes(void) { return bytes_used; }
unsigned ls_lap_api_bad_frees(void) { return bad_frees; }
const char *ls_lap_api_error(void) { return s.error; }
void *ls_lap_malloc(size_t n) {
    if (!s.generation || !n)
        return NULL;
    if (n > LS_LAP_MAX_MEMORY || n + sizeof(block) > LS_LAP_MAX_MEMORY - bytes_used) {
        error("app memory cap is 4 MB");
        return NULL;
    }
    block *b = ALLOC(sizeof(block) + n);
    if (!b) {
        error("PSRAM allocation refused");
        return NULL;
    }
    b->h.next = s.blocks;
    b->h.size = n;
    b->h.gen = s.generation;
    s.blocks = b;
    bytes_used += n + sizeof(block);
    return b + 1;
}
void *ls_lap_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) {
        error("allocation overflow");
        return NULL;
    }
    void *p = ls_lap_malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}
static block **find(void *p) {
    block **b = &s.blocks;
    while (*b && ((void *)(*b + 1) != p || (*b)->h.gen != s.generation))
        b = &(*b)->h.next;
    return b;
}
void ls_lap_free(void *p) {
    if (!p)
        return;
    block **prev = find(p), *b = *prev;
    if (!b) {
        bad_frees++;
        return;
    }
    *prev = b->h.next;
    bytes_used -= b->h.size + sizeof(block);
    DEALLOC(b);
}
void *ls_lap_realloc(void *p, size_t n) {
    if (!p)
        return ls_lap_malloc(n);
    block *b = *find(p);
    if (!b) {
        bad_frees++;
        return NULL;
    }
    if (!n) {
        ls_lap_free(p);
        return NULL;
    }
    if (n > LS_LAP_MAX_MEMORY ||
        n + sizeof(block) > LS_LAP_MAX_MEMORY - (bytes_used - b->h.size - sizeof(block))) {
        error("app memory cap is 4 MB");
        return NULL;
    }
    block **prev = find(p);
    size_t old = b->h.size;
    block *q = RESIZE(b, n + sizeof(block));
    if (!q) {
        error("PSRAM allocation refused");
        return NULL;
    }
    q->h.size = n;
    *prev = q;
    bytes_used = bytes_used - old + n;
    return q + 1;
}
void ls_lap_api_cleanup(void) {
    ls_lap_api_draw_end();
    s.callback = false;
    while (s.blocks)
        ls_lap_free(s.blocks + 1);
    s.files = NULL;
    memset(s.fd, 0, sizeof(s.fd));
    s.generation = 0;
}
static bool draw(void) { return inside() && s.surface; }
static int16_t cell(int16_t c) {
    return ((c >= 32 && c <= 126) || (c >= 0x80 && c <= 0x9f) || (c >= 0xc0 && c <= 0xff) ||
            (c >= 0x2800 && c <= 0x28ff))
               ? c
               : '?';
}
int lap_cols(void) { return draw() ? s.area.w : 0; }
int lap_rows(void) { return draw() ? s.area.h : 0; }
void lap_put_cell(int x, int y, int16_t c, uint8_t attr) {
    if (!draw() || x < 0 || y < 0 || x >= s.area.w || y >= s.area.h)
        return;
    int ax = x + s.area.x, ay = y + s.area.y;
    s.surface->back[(size_t)ay * s.surface->w + ax] = (tui_cell){cell(c), attr};
}
void lap_clear(uint8_t attr) {
    if (draw())
        tui_fill(s.surface, s.area, ' ', attr);
}
void lap_put_str(int x, int y, const char *str, uint8_t attr) {
    if (!draw() || !str || y < 0 || y >= s.area.h || x >= s.area.w || x < -4096)
        return;
    tui_put_str(s.surface, s.area, s.area.x + x, s.area.y + y, str, attr);
}
void lap_fill(int x, int y, int w, int h, int16_t c, uint8_t a) {
    if (!draw() || w <= 0 || h <= 0)
        return;
    int64_t x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    int l = x < 0 ? 0 : x, t = y < 0 ? 0 : y, r = x1 > s.area.w ? s.area.w : (int)x1,
        b = y1 > s.area.h ? s.area.h : (int)y1;
    for (int row = t; row < b; row++)
        for (int col = l; col < r; col++)
            lap_put_cell(col, row, c, a);
}
void lap_hline(int x, int y, int len, int16_t c, uint8_t a) { lap_fill(x, y, len, 1, c, a); }
void lap_vline(int x, int y, int len, int16_t c, uint8_t a) { lap_fill(x, y, 1, len, c, a); }
void lap_box(int x, int y, int w, int h, const char *title, uint8_t a) {
    if (!draw() || w < 2 || h < 2 || (int64_t)x + w > INT_MAX || (int64_t)y + h > INT_MAX)
        return;
    /* tui_box has no caller clip; use it only when wholly inside the app. */
    if (x >= 0 && y >= 0 && w <= s.area.w - x && h <= s.area.h - y) {
        tui_box(s.surface, tui_rect_make(s.area.x + x, s.area.y + y, w, h), title, a);
        return;
    }
    lap_hline(x, y, w, '-', a);
    lap_hline(x, y + h - 1, w, '-', a);
    lap_vline(x, y, h, '|', a);
    lap_vline(x + w - 1, y, h, '|', a);
    lap_put_cell(x, y, '+', a);
    lap_put_cell(x + w - 1, y, '+', a);
    lap_put_cell(x, y + h - 1, '+', a);
    lap_put_cell(x + w - 1, y + h - 1, '+', a);
}
bool lap_big_char(int x, int y, char c, uint8_t a) {
    if (!draw() || !ls_glyph_has(c))
        return false;
    /* Render into a tiny local surface so even a partially visible glyph clips. */
    tui_cell cells[15], front[15];
    tui_surface sf;
    tui_surface_setup(&sf, cells, front, 3, 5);
    tui_frame_begin(&sf);
    ls_glyph_draw(&sf, tui_rect_make(0, 0, 3, 5), c, a);
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++)
            if ((int64_t)x + col <= INT_MAX && (int64_t)y + row <= INT_MAX)
                lap_put_cell(x + col, y + row, (uint8_t)cells[row * 3 + col].ch,
                             cells[row * 3 + col].attr);
    return true;
}
void lap_image(int x, int y, int w, int h, const uint16_t *p, int pw, int ph, uint32_t serial) {
    if (!draw() || !p || w <= 0 || h <= 0 || pw <= 0 || ph <= 0 ||
        (size_t)pw > LS_LAP_MAX_MEMORY / 2 / (size_t)ph)
        return;
    int64_t right = (int64_t)x + w, bottom = (int64_t)y + h;
    int l = x < 0 ? 0 : x, t = y < 0 ? 0 : y, r = right > s.area.w ? s.area.w : (int)right,
        b = bottom > s.area.h ? s.area.h : (int)bottom;
    if (r <= l || b <= t)
        return;
    /* Existing image API scales the full source into the clipped cell rectangle. */
    tui_rect rct = tui_rect_make(s.area.x + l, s.area.y + t, r - l, b - t);
    bool changed = !s.image_active || s.image_pixels != p || s.image_w != pw || s.image_h != ph ||
                   s.image_serial != serial || memcmp(&s.image_area, &rct, sizeof(rct));
    /* Stationary ring mode uses the shared image renderer without allocating its comparison copy.
     */
    ls_tui_image_ring(rct, p, pw, ph, 0, 0, changed ? -1 : 0, changed ? -1 : 0, serial);
    s.image_called = s.image_active = true;
    s.image_area = rct;
    s.image_pixels = p;
    s.image_w = pw;
    s.image_h = ph;
    s.image_serial = serial;
    tui_fill(s.surface, rct, LS_TUI_IMAGE_CELL, TUI_DEFAULT_ATTR);
}
/* COMPASS's packing (ls_dots_blit): each cell keeps its two commonest
   colours, the commonest as ink, ties to the brighter index. */
void lap_dots(int x, int y, int w, int h, const uint8_t *dots) {
    if (!draw() || !dots || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return;
    const int dw = w * 2;
    for (int r = 0; r < h; r++) {
        if (y + r < 0 || y + r >= s.area.h)
            continue;
        const uint8_t *row0 = dots + (size_t)(3 * r) * dw;
        for (int c = 0; c < w; c++) {
            if (x + c < 0 || x + c >= s.area.w)
                continue;
            uint8_t col[6];
            for (int k = 0; k < 6; k++) {
                const uint8_t d = row0[(size_t)(k / 2) * dw + 2 * c + (k & 1)];
                col[k] = d == LAP_DOT_CLEAR ? 0 : d & 15;
            }
            int fg = col[0], bg = 0, bits = 0x3F;
            if (col[1] != fg || col[2] != fg || col[3] != fg || col[4] != fg || col[5] != fg) {
                /* A count per colour; ties go to the brighter ink, and the
                   paper to the first colour met with the most dots. */
                uint8_t count[16] = {0};
                for (int k = 0; k < 6; k++)
                    count[col[k]]++;
                for (int k = 1; k < 6; k++)
                    if (count[col[k]] > count[fg] || (count[col[k]] == count[fg] && col[k] > fg))
                        fg = col[k];
                int paper = -1;
                for (int k = 0; k < 6; k++)
                    if (col[k] != fg && (paper < 0 || count[col[k]] > count[paper]))
                        paper = col[k];
                bg = paper >= 0 ? paper : 0;
                bits = 0;
                for (int k = 0; k < 6; k++)
                    if (col[k] == fg)
                        bits |= 1 << k;
            }
            lap_put_cell(x + c, y + r, LAP_SEXT(bits), LAP_ATTR(fg, bg));
        }
    }
}
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "ls_theme.h"
void lap_put_glass(int x, int y, const char *str, uint8_t attr) {
    if (!draw() || !str || y < 0 || y >= s.area.h)
        return;
    for (int i = 0; str[i] && x + i < s.area.w; i++)
        if (x + i >= 0)
            ls_tui_put_glass(s.surface, s.area, s.area.x + x + i, s.area.y + y,
                             cell((unsigned char)str[i]), attr);
}
void lap_cell_px(int *w, int *h) {
    int cols, rows, cw = 10, ch = 17;
    if (inside())
        ls_tui_geometry(&cols, &rows, &cw, &ch);
    if (w)
        *w = cw;
    if (h)
        *h = ch;
}
uint16_t lap_palette(int c) {
    const ls_tui_theme_t *t = ls_tui_theme_effective(ls_tui_get_theme(), ls_tui_daylight());
    return t && inside() ? t->palette[c & 15] : 0;
}
#else
void lap_put_glass(int x, int y, const char *str, uint8_t attr) { lap_put_str(x, y, str, attr); }
void lap_cell_px(int *w, int *h) {
    if (w)
        *w = 10;
    if (h)
        *h = 17;
}
uint16_t lap_palette(int c) {
    static const uint16_t P[16] = {0x0000, 0xB249, 0x4C6B, 0xB447, 0x3B74, 0x8AD4, 0x3C73, 0xAD97,
                                   0x3A09, 0xFB4D, 0x6F0F, 0xFE4A, 0x6DBF, 0xDC5F, 0x5F1C, 0xF7BF};
    return inside() ? P[c & 15] : 0;
}
#endif
uint32_t lap_millis(void) { return inside() ? now() - s.started : 0; }
uint32_t lap_frame(void) { return inside() ? s.frame : 0; }

/* Receiver has no reader lock. Copy each record before deriving public data;
 * scratch stays in PSRAM and is used only on the serialized app callback task. */
int lap_aircraft(lap_aircraft_t *out, int max) {
    EXT_RAM_BSS_ATTR static lap_aircraft_t sorted[ADSB_MAX_TRACKED], tmp;
    EXT_RAM_BSS_ATTR static adsb_aircraft_t copy;
    lap_gps_t gps;
    if (!inside() || !out || max <= 0) return 0;
    const bool fix = lap_gps(&gps) && gps.fix;
    const int64_t us = esp_timer_get_time();
    int n = 0;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        const adsb_aircraft_t *src = adsb_state_get(i);
        if (!src) continue;
        copy = *src;
        if (!copy.active || !copy.icao) continue;
        memset(&tmp, 0, sizeof(tmp));
        tmp.icao = copy.icao & 0xFFFFFF;
        memcpy(tmp.callsign, copy.callsign, 8);
        tmp.has_pos = copy.pos_valid;
        tmp.has_alt = copy.altitude_valid;
        tmp.has_speed = copy.velocity_valid;
        tmp.has_track = copy.heading_valid && copy.heading_is_track;
        tmp.has_vrate = copy.vert_rate_valid;
        tmp.lat = copy.lat; tmp.lon = copy.lon;
        tmp.alt_ft = copy.altitude;
        tmp.speed_kt = (int16_t)copy.velocity;
        tmp.track_deg = (int16_t)copy.heading;
        tmp.vrate_fpm = (int16_t)copy.vert_rate;
        tmp.squawk = (uint16_t)adsb_state_squawk(i);
        tmp.emergency = (uint8_t)adsb_state_emergency(i, NULL);
        if (copy.emitter_tc >= 1 && copy.emitter_tc <= 4 && copy.emitter_ca > 0 && copy.emitter_ca <= 7) {
            tmp.category[0] = 'A' + 4 - copy.emitter_tc;
            tmp.category[1] = '0' + copy.emitter_ca;
        }
        int64_t age = us > copy.last_seen_us ? (us - copy.last_seen_us) / 1000 : 0;
        tmp.age_ms = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
        tmp.range_km = -1;
        if (fix && tmp.has_pos) {
            const float rad = 0.01745329252f;
            const float a = (float)gps.lat * rad, b = copy.lat * rad;
            const float dl = ((float)copy.lon - (float)gps.lon) * rad;
            const float sn = sinf((b - a) * 0.5f), se = sinf(dl * 0.5f);
            const float h = fminf(1, fmaxf(0, sn * sn + cosf(a) * cosf(b) * se * se));
            tmp.range_km = 12742.0f * atan2f(sqrtf(h), sqrtf(1 - h));
            tmp.bearing_deg = fmodf(atan2f(sinf(dl) * cosf(b),
                cosf(a) * sinf(b) - sinf(a) * cosf(b) * cosf(dl)) * 57.29578f + 360, 360);
        }
        int j = n;
        const float key = tmp.range_km < 0 ? INFINITY : tmp.range_km;
        while (j && (sorted[j-1].range_km < 0 ? INFINITY : sorted[j-1].range_km) > key) {
            sorted[j] = sorted[j-1]; j--;
        }
        sorted[j] = tmp; n++;
    }
    if (n > max) n = max;
    memcpy(out, sorted, (size_t)n * sizeof(*out));
    return n;
}

int lap_aircraft_track(uint32_t icao, float *lat, float *lon, int32_t *alt_ft, int max) {
    EXT_RAM_BSS_ATTR static adsb_trail_pt_t trail[ADSB_TRAIL_N];
    if (!inside() || !lat || !lon || !alt_ft || max <= 0 || icao > 0xFFFFFF) return 0;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        if (!a || !a->active || a->icao != icao) continue;
        int n = adsb_state_trail(i, trail, max > ADSB_TRAIL_N ? ADSB_TRAIL_N : max);
        for (int j = 0; j < n; j++) {
            lat[j] = trail[j].lat; lon[j] = trail[j].lon; alt_ft[j] = trail[j].altitude;
        }
        return n;
    }
    return 0;
}

void lap_aircraft_demo(bool on) {
    if (inside()) adsb_demo_set(on ? 8 : 0);
}
uint32_t lap_random(void) {
#ifdef ESP_PLATFORM
    return inside() ? esp_random() : 0;
#else
    static uint32_t x = 2463534242u; /* the bench: repeatable */
    if (!inside())
        return 0;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
#endif
}
void lap_exit(void) {
    if (inside())
        s.exit = true;
}
void lap_log(const char *fmt, ...) {
    if (!inside() || !fmt)
        return;
    unsigned head = atomic_load_explicit(&s.log_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&s.log_tail, memory_order_acquire);
    if (head - tail >= 8) {
        error("app log queue full");
        return;
    }
    char *line = s.log[head % 8];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, 160, fmt, args);
    va_end(args);
    for (char *p = line; *p; p++)
        if ((unsigned char)*p < 32 || *p == 127)
            *p = ' ';
    atomic_store_explicit(&s.log_head, head + 1, memory_order_release);
    if (wake_io)
        wake_io();
}
void ls_lap_api_log_service(void) {
    unsigned tail = atomic_load_explicit(&s.log_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&s.log_head, memory_order_acquire);
    while (tail != head) {
        printf("[app %s] %s\n", s.id, s.log[tail % 8]);
        atomic_store_explicit(&s.log_tail, ++tail, memory_order_release);
    }
}

bool lap_value_text(const char *path, char *out, size_t cap) {
    ls_val_t v;
    const char *unit;
    if (!inside() || !path || !out || !cap || !ls_value_read(path, &v, &unit))
        return false;
    switch (v.kind) {
    case LS_VAL_TEXT:
        snprintf(out, cap, "%s", v.s ? v.s : "");
        break;
    case LS_VAL_FLOAT:
        snprintf(out, cap, "%.6g", (double)v.f);
        break;
    case LS_VAL_BOOL:
    case LS_VAL_INT:
        snprintf(out, cap, "%ld", v.i);
        break;
    default:
        return false;
    }
    return true;
}
bool lap_value_float(const char *path, float *out) {
    ls_val_t v;
    const char *unit;
    if (!inside() || !path || !out || !ls_value_read(path, &v, &unit))
        return false;
    if (v.kind == LS_VAL_FLOAT)
        *out = v.f;
    else if (v.kind == LS_VAL_INT || v.kind == LS_VAL_BOOL)
        *out = (float)v.i;
    else
        return false;
    return true;
}
void lap_sound(int which) {
#ifdef ESP_PLATFORM
    /* Existing notification worker honours board volume/mute. */
    if (!inside())
        return;
    switch (which) {
    case LAP_SOUND_KEY:
        snd_notification_start(SETTINGS_NOTIFY_RADIOKEY);
        break;
    case LAP_SOUND_ROGER:
        snd_notification_start(SETTINGS_NOTIFY_ROGER);
        break;
    case LAP_SOUND_CHIRP:
        snd_notification_start(SETTINGS_NOTIFY_CHIRP);
        break;
    case LAP_SOUND_ERROR:
        snd_notification_start(SETTINGS_NOTIFY_ALERT);
        break;
    default:
        break;
    }
#else
    (void)which;
#endif
}
static bool filename(const char *name) { return ls_lap_plain_name(name, 48); }

static cache *lookup(const char *name) {
    for (cache *f = s.files; f; f = f->next)
        if (ls_lap_name_equal(name, f->name))
            return f;
    return NULL;
}
static void path(char out[128], const char *name) {
    snprintf(out, 128, "%.40s/%.31s/%.48s", s.data_root, s.id, name ? name : "");
}
bool ls_lap_api_files_load(const char *root) {
    if (!root || strlen(root) > 40) {
        error("app data root too long");
        return false;
    }
    snprintf(s.data_root, sizeof(s.data_root), "%s", root);
    char dir[128];
    path(dir, NULL);
    if (MAKE_DIR(root) && errno != EEXIST) {
        error("cannot create apps_data");
        return false;
    }
    if (MAKE_DIR(dir) && errno != EEXIST) {
        error("cannot create app data folder");
        return false;
    }
    if (!ls_lap_recover_dir(dir)) {
        error("app data recovery failed");
        return false;
    }
    DIR *d = opendir(dir);
    if (!d) {
        error("cannot read app data folder");
        return false;
    }
    struct dirent *e;
    bool ok = true;
    while (ok && (e = readdir(d))) {
        if (!filename(e->d_name))
            continue;
        char full[128];
        path(full, e->d_name);
        struct stat st;
        if (stat(full, &st) || !S_ISREG(st.st_mode))
            continue;
        cache *c = ls_lap_calloc(1, sizeof(cache));
        if (!c) {
            ok = false;
            break;
        }
        memcpy(c->name, e->d_name, strlen(e->d_name) + 1);
        c->next = s.files;
        s.files = c;
        if (st.st_size < 0 || (uint64_t)st.st_size > LS_LAP_MAX_MEMORY) {
            error("app data exceeds 4 MB cache");
            ok = false;
            break;
        }
        c->size = c->capacity = (size_t)st.st_size;
        c->data = c->size ? ls_lap_malloc(c->size) : NULL;
        if (c->size && !c->data) {
            ok = false;
            break;
        }
        FILE *f = fopen(full, "rb");
        ok = f && fread(c->data, 1, c->size, f) == c->size;
        if (f)
            fclose(f);
        if (!ok)
            error("cannot cache app data");
    }
    closedir(d);
    return ok;
}
bool ls_lap_api_files_flush(void) {
    bool ok = true;
    memset(s.fd, 0, sizeof(s.fd));
    for (cache *c = s.files; c; c = c->next)
        if (c->dirty) {
            char full[128];
            path(full, c->name);
            if (c->removed) {
                if (remove(full) && errno != ENOENT)
                    ok = false;
            } else {
                /* Stage in the app's folder before replacing the previous file. */
                char temp[144];
                snprintf(temp, sizeof(temp), "%s/%s/.lap-write", s.data_root, s.id);
                FILE *f = fopen(temp, "wb");
                bool wrote = f && fwrite(c->data, 1, c->size, f) == c->size;
                if (f && fclose(f))
                    wrote = false;
                if (wrote)
                    wrote = ls_lap_replace(temp, full);
                if (!wrote) {
                    remove(temp);
                    ok = false;
                }
            }
        }
    if (!ok)
        error("app data flush failed");
    return ok;
}
int lap_file_open(const char *name, const char *mode) {
    if (!inside() || !filename(name) || !mode || !mode[0] || mode[1] || !strchr("rwa", mode[0]))
        return -1;
    int fd = 0;
    while (fd < 4 && s.fd[fd].file)
        fd++;
    if (fd == 4)
        return -1;
    cache *c = lookup(name);
    if ((!c || c->removed) && *mode == 'r')
        return -1;
    if (!c) {
        c = ls_lap_calloc(1, sizeof(cache));
        if (!c)
            return -1;
        snprintf(c->name, sizeof(c->name), "%s", name);
        c->next = s.files;
        s.files = c;
    }
    if (*mode == 'w' || c->removed) {
        c->size = 0;
        c->dirty = true;
    }
    if (*mode == 'a')
        c->dirty = true;
    c->removed = false;
    s.fd[fd] = (handle){c, *mode == 'a' ? c->size : 0, *mode};
    return fd;
}
int lap_file_read(int fd, void *buf, size_t n) {
    if (!inside() || fd < 0 || fd >= 4 || !buf || !s.fd[fd].file || s.fd[fd].mode != 'r')
        return -1;
    handle *h = &s.fd[fd];
    if (n > INT_MAX)
        n = INT_MAX;
    size_t avail = h->pos < h->file->size ? h->file->size - h->pos : 0;
    if (n > avail)
        n = avail;
    if (n)
        memcpy(buf, h->file->data + h->pos, n);
    h->pos += n;
    return (int)n;
}
int lap_file_write(int fd, const void *buf, size_t n) {
    if (!inside() || fd < 0 || fd >= 4 || !buf || !s.fd[fd].file || s.fd[fd].mode == 'r' ||
        n > INT_MAX)
        return -1;
    handle *h = &s.fd[fd];
    cache *c = h->file;
    if (h->mode == 'a')
        h->pos = c->size;
    if (n > LS_LAP_MAX_MEMORY || h->pos > LS_LAP_MAX_MEMORY - n) {
        error("app memory cap is 4 MB");
        return -1;
    }
    size_t end = h->pos + n;
    if (end > c->capacity) {
        unsigned char *p = ls_lap_realloc(c->data, end);
        if (!p)
            return -1;
        c->data = p;
        c->capacity = end;
    }
    if (n)
        memcpy(c->data + h->pos, buf, n);
    h->pos = end;
    if (end > c->size)
        c->size = end;
    c->dirty = true;
    return (int)n;
}
void lap_file_close(int fd) {
    if (inside() && fd >= 0 && fd < 4)
        memset(&s.fd[fd], 0, sizeof(handle));
}
bool lap_file_remove(const char *name) {
    if (!inside() || !filename(name))
        return false;
    cache *c = lookup(name);
    if (!c || c->removed)
        return false;
    for (int i = 0; i < 4; i++)
        if (s.fd[i].file == c)
            return false;
    c->removed = true;
    c->dirty = true;
    return true;
}
long lap_file_size(const char *name) {
    if (!inside() || !filename(name))
        return -1;
    cache *c = lookup(name);
    return c && !c->removed ? (long)c->size : -1;
}

#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
/* Compiler helpers in the board libgcc; the SDK imports aliases to avoid non-PIC libgcc.a. */
extern double __adddf3(double, double);
extern double __subdf3(double, double);
extern double __muldf3(double, double);
extern double __divdf3(double, double);
extern double __extendsfdf2(float);
extern float __truncdfsf2(double);
extern int __fixdfsi(double);
extern unsigned __fixunsdfsi(double);
extern long long __fixdfdi(double);
extern unsigned long long __fixunsdfdi(double);
extern double __floatsidf(int);
extern double __floatunsidf(unsigned);
extern double __floatdidf(long long);
extern double __floatundidf(unsigned long long);
extern int __eqdf2(double, double);
extern int __nedf2(double, double);
extern int __ltdf2(double, double);
extern int __ledf2(double, double);
extern int __gtdf2(double, double);
extern int __gedf2(double, double);
extern int __unorddf2(double, double);
extern long long __divdi3(long long, long long);
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern long long __moddi3(long long, long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);

#else
static void host_helper(void) {}
#define __adddf3 host_helper
#define __subdf3 host_helper
#define __muldf3 host_helper
#define __divdf3 host_helper
#define __extendsfdf2 host_helper
#define __truncdfsf2 host_helper
#define __fixdfsi host_helper
#define __fixunsdfsi host_helper
#define __fixdfdi host_helper
#define __fixunsdfdi host_helper
#define __floatsidf host_helper
#define __floatunsidf host_helper
#define __floatdidf host_helper
#define __floatundidf host_helper
#define __eqdf2 host_helper
#define __nedf2 host_helper
#define __ltdf2 host_helper
#define __ledf2 host_helper
#define __gtdf2 host_helper
#define __gedf2 host_helper
#define __unorddf2 host_helper
#define __divdi3 host_helper
#define __udivdi3 host_helper
#define __moddi3 host_helper
#define __umoddi3 host_helper
#endif

#define EXPORT(name, fn) {name, (uintptr_t)(fn)}
/* Kept sorted and cross-checked against the public header and SDK exports.txt. */
static const ls_lap_export_t exports[] = {
#include "ls_lap_exports.inc"
};
#undef EXPORT
const ls_lap_export_t *ls_lap_exports(size_t *count) {
    if (count)
        *count = sizeof(exports) / sizeof(exports[0]);
    return exports;
}
uintptr_t ls_lap_resolve(const char *name) {
    if (!name)
        return 0;
    size_t lo = 0, hi = sizeof(exports) / sizeof(exports[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(name, exports[mid].name);
        if (!c)
            return exports[mid].address;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    if (!s.missing[0])
        snprintf(s.missing, sizeof(s.missing), "needs %.88s", name);
    return 0;
}
const char *ls_lap_missing_symbol(void) { return s.missing; }
