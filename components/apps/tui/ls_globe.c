/* The globe: see ls_globe.h. */
#include "ls_globe.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ls_map_ink.h"
#include "ls_tui_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ls_cold.h"
#if __has_include("freertos/idf_additions.h")
#include "freertos/idf_additions.h"
#endif

/* A character cell is about 0.87 as wide per dot as it is tall per dot
   (two dots across, three down, in a cell about 1:1.75): y is squeezed by
   this so the globe comes out round. */
#define DOT_ASPECT 0.87f
#define GRID_STEP  30      /* graticule, degrees  */
#define FILL_ROWS  6       /* fill rows per frame  */
#define MAX_CELLS  (160 * 100)

typedef struct {
    char     code[8], name[32];
    uint8_t  level;
    int16_t  bb[4];          /* west south east north, deg*100 */
    uint32_t ring0;
    uint16_t nring;
    float    clat, clon;     /* centre: mean of its points, longitude as an angle */
} region_t;
typedef struct { uint32_t p0, n; } ring_t;
typedef struct { float sl, cl, sx, cx; } trig_t;   /* sin/cos lat, sin/cos lon */

typedef struct {
    region_t *reg; ring_t *ring; int16_t *pt; trig_t *trig; int16_t *proj;
    uint32_t nreg, nring, npt;
} bundle_t;

static bundle_t s_b;                 /* in use by the UI */
static bundle_t s_next;              /* handed over by the loader */
static volatile int s_state, s_next_ready;
static char s_path[96];
static long s_loaded_size;

/* view: drawn (eases) and target */
static float s_lat = 38.5f, s_lon = -96.0f, s_zoom = 1.0f;
static float s_tlat = 38.5f, s_tlon = -96.0f, s_tzoom = 1.0f;
static bool  s_proj_ok;
static float s_plat = 999, s_plon, s_pzoom;
static int   s_pw, s_ph;

/* last draw, for hits */
static tui_rect s_area;
static float s_R, s_cx, s_cy;

/* per-cell region (index+1, 0 none) for fills, filled a few rows a frame */
static uint16_t *s_cellreg;
static int s_fill_row, s_fill_w, s_fill_h;

static void bundle_free(bundle_t *b)
{
    heap_caps_free(b->reg); heap_caps_free(b->ring); heap_caps_free(b->pt);
    heap_caps_free(b->trig); heap_caps_free(b->proj);
    memset(b, 0, sizeof(*b));
}

static void *ps(size_t n) { return heap_caps_calloc(1, n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t  i16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

static bool parse(const uint8_t *d, size_t n, bundle_t *b)
{
    if (n < 20 || memcmp(d, "LSOUT1\0\0", 8)) return false;
    const uint32_t nr = u32(d + 8), ng = u32(d + 12), np = u32(d + 16);
    const size_t need = 20 + (size_t)nr * 58 + (size_t)ng * 8 + (size_t)np * 4;   /* region rows are 58 bytes */
    if (nr > 4096 || ng > 65536 || np > 400000 || need > n) return false;
    b->reg = ps(nr * sizeof(region_t)); b->ring = ps(ng * sizeof(ring_t));
    b->pt = ps(np * 4); b->trig = ps(np * sizeof(trig_t)); b->proj = ps(np * 4);
    if (!b->reg || !b->ring || !b->pt || !b->trig || !b->proj) return false;
    const uint8_t *p = d + 20;
    for (uint32_t i = 0; i < nr; i++, p += 58) {
        region_t *r = &b->reg[i];
        memcpy(r->code, p, 8); r->code[7] = 0;
        r->level = p[8];
        for (int k = 0; k < 4; k++) r->bb[k] = i16(p + 10 + 2 * k);
        r->ring0 = u32(p + 18); r->nring = (uint16_t)(p[22] | p[23] << 8);
        memcpy(r->name, p + 26, 32); r->name[31] = 0;
        if (r->ring0 + r->nring > ng) return false;
    }
    for (uint32_t i = 0; i < ng; i++, p += 8) {
        b->ring[i].p0 = u32(p); b->ring[i].n = u32(p + 4);
        if (b->ring[i].p0 + b->ring[i].n > np) return false;
    }
    memcpy(b->pt, p, np * 4);
    for (uint32_t i = 0; i < nr; i++) {
        /* Alaska's box runs -180..180 across the date line: average the
           longitude as an angle, not as a number. */
        region_t *r = &b->reg[i];
        double sx = 0, cx = 0, sy = 0; uint32_t cnt = 0;
        for (uint32_t k = 0; k < r->nring; k++) {
            const ring_t *g = &b->ring[r->ring0 + k];
            for (uint32_t j = 0; j < g->n; j++) {
                const double lo = b->pt[2 * (g->p0 + j)] * 0.01 * M_PI / 180.0;
                sx += sin(lo); cx += cos(lo); sy += b->pt[2 * (g->p0 + j) + 1] * 0.01; cnt++;
            }
        }
        r->clat = cnt ? (float)(sy / cnt) : 0; r->clon = (float)(atan2(sx, cx) * 180.0 / M_PI);
    }
    for (uint32_t i = 0; i < np; i++) {
        const float lon = b->pt[2 * i] * 0.01f * (float)M_PI / 180.0f;
        const float lat = b->pt[2 * i + 1] * 0.01f * (float)M_PI / 180.0f;
        b->trig[i] = (trig_t){ sinf(lat), cosf(lat), sinf(lon), cosf(lon) };
    }
    b->nreg = nr; b->nring = ng; b->npt = np;
    return true;
}

static const char *TAG = "globe";
static int64_t s_retry_us;

static void load_work(void)
{
    bundle_t b = {0};
    bool ok = false;
    const char *why = "cannot open";
    FILE *f = fopen(s_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *d = n > 0 && n < 4 * 1024 * 1024 ? ps((size_t)n) : NULL;
        why = d ? "short read" : "no memory for the file";
        if (d && fread(d, 1, (size_t)n, f) == (size_t)n) {
            ok = parse(d, (size_t)n, &b);
            why = "not an LSOUT1 file";
            if (ok) s_loaded_size = n;
        }
        heap_caps_free(d);
        fclose(f);
    }
    if (ok) ESP_LOGI(TAG, "%u regions, %u rings, %u points", (unsigned)b.nreg, (unsigned)b.nring, (unsigned)b.npt);
    else { ESP_LOGW(TAG, "%s: %s", s_path, why); s_retry_us = esp_timer_get_time() + 15000000; }
    if (ok) { s_next = b; s_next_ready = 1; }
    else { bundle_free(&b); s_state = s_b.nreg ? LS_GLOBE_READY : LS_GLOBE_FAILED; }
}
LS_COLD static void loader(void *arg) { (void)arg; load_work(); vTaskDeleteWithCaps(NULL); }

int ls_globe_state(void) { return s_state; }

LS_COLD int ls_globe_load(const char *path)
{
    if (s_next_ready) {           /* swap on the UI task, never under a draw */
        bundle_free(&s_b); s_b = s_next; memset(&s_next, 0, sizeof(s_next));
        s_next_ready = 0; s_state = LS_GLOBE_READY; s_proj_ok = false; ls_globe_refill();
    }
    if (s_state == LS_GLOBE_LOADING) return s_state;
    /* The card is looked at every two seconds at most, never every frame. */
    static int64_t s_stat_us;
    const int64_t now = esp_timer_get_time();
    if (s_stat_us && now - s_stat_us < 2000000) return s_state;
    s_stat_us = now;
    snprintf(s_path, sizeof(s_path), "%s", path ? path : LS_GLOBE_PATH);
    struct stat st;
    if (stat(s_path, &st) != 0) { if (!s_b.nreg) s_state = LS_GLOBE_NONE; return s_state; }
    if (s_b.nreg && st.st_size == s_loaded_size) return s_state;
    if (s_state == LS_GLOBE_FAILED && esp_timer_get_time() < s_retry_us) return s_state;
    s_state = LS_GLOBE_LOADING;
    s_loaded_size = st.st_size;
#ifdef LS_TILES_HOST
    load_work();                  /* lssim: no tasks run, load in place */
    return ls_globe_load(path);
#endif
    if (xTaskCreatePinnedToCoreWithCaps(loader, "globe_load", 6144, NULL, 2, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    { ESP_LOGW(TAG, "no task for the loader"); s_state = LS_GLOBE_FAILED; s_retry_us = esp_timer_get_time() + 15000000; }
    return s_state;
}

void ls_globe_target(float lat, float lon, float zoom)
{
    if (lat > 85) lat = 85;
    if (lat < -85) lat = -85;
    while (lon > 180) lon -= 360;
    while (lon < -180) lon += 360;
    s_tlat = lat; s_tlon = lon; s_tzoom = zoom < 1.0f ? 1.0f : zoom > 40.0f ? 40.0f : zoom;
}
void ls_globe_view(float *lat, float *lon, float *zoom)
{
    if (lat) *lat = s_tlat;
    if (lon) *lon = s_tlon;
    if (zoom) *zoom = s_tzoom;
}
bool ls_globe_moving(void)
{
    float dl = s_tlon - s_lon;
    if (dl > 180) dl -= 360;
    if (dl < -180) dl += 360;
    return fabsf(s_tlat - s_lat) > 0.05f || fabsf(dl) > 0.05f || fabsf(s_tzoom - s_zoom) > 0.01f;
}
void ls_globe_refill(void) { s_fill_row = 0; s_fill_w = 0; }

/* One step of easing toward the target: a quarter of the way each frame,
   along the short way round. */
static void ease(void)
{
    float dl = s_tlon - s_lon;
    if (dl > 180) dl -= 360;
    if (dl < -180) dl += 360;
    if (!ls_globe_moving()) { s_lat = s_tlat; s_lon = s_tlon; s_zoom = s_tzoom; return; }
    s_lat += (s_tlat - s_lat) * 0.3f;
    s_lon += dl * 0.3f;
    if (s_lon > 180) s_lon -= 360;
    if (s_lon < -180) s_lon += 360;
    s_zoom += (s_tzoom - s_zoom) * 0.3f;
}

typedef struct { float sl0, cl0, sx0, cx0; } center_t;

static bool project(const center_t *c, float sl, float cl, float sx, float cx, float *x, float *y)
{
    const float cdl = cx * c->cx0 + sx * c->sx0;          /* cos(lon-lon0) */
    const float sdl = sx * c->cx0 - cx * c->sx0;          /* sin(lon-lon0) */
    const float cosc = c->sl0 * sl + c->cl0 * cl * cdl;
    if (cosc < 0.0f) return false;
    *x = s_cx + s_R * cl * sdl;
    *y = s_cy - s_R * DOT_ASPECT * (c->cl0 * sl - c->sl0 * cl * cdl);
    return true;
}

static center_t center_now(void)
{
    const float la = s_lat * (float)M_PI / 180.0f, lo = s_lon * (float)M_PI / 180.0f;
    return (center_t){ sinf(la), cosf(la), sinf(lo), cosf(lo) };
}

/* Screen dot back to lat/lon (degrees); false off the globe. */
static bool unproject(float px, float py, float *lat, float *lon)
{
    const float x = (px - s_cx) / s_R, y = (s_cy - py) / (s_R * DOT_ASPECT);
    const float rho = sqrtf(x * x + y * y);
    if (rho > 1.0f) return false;
    const center_t c = center_now();
    if (rho < 1e-6f) { *lat = s_lat; *lon = s_lon; return true; }
    const float cc = asinf(rho), sc = sinf(cc), cs = cosf(cc);
    *lat = asinf(cs * c.sl0 + y * sc * c.cl0 / rho) * 180.0f / (float)M_PI;
    *lon = s_lon + atan2f(x * sc, rho * c.cl0 * cs - y * c.sl0 * sc) * 180.0f / (float)M_PI;
    if (*lon > 180) *lon -= 360;
    if (*lon < -180) *lon += 360;
    return true;
}

static bool in_region(const region_t *r, float lat, float lon)
{
    const int16_t qx = (int16_t)lrintf(lon * 100), qy = (int16_t)lrintf(lat * 100);
    if (qx < r->bb[0] || qx > r->bb[2] || qy < r->bb[1] || qy > r->bb[3]) return false;
    bool in = false;
    for (uint32_t k = 0; k < r->nring; k++) {
        const ring_t *g = &s_b.ring[r->ring0 + k];
        const int16_t *p = &s_b.pt[2 * g->p0];
        for (uint32_t i = 0, j = g->n - 1; i < g->n; j = i++) {
            const int xi = p[2 * i], yi = p[2 * i + 1], xj = p[2 * j], yj = p[2 * j + 1];
            if ((yi > qy) != (yj > qy) &&
                qx < (float)(xj - xi) * (qy - yi) / (float)(yj - yi) + xi) in = !in;
        }
    }
    return in;
}

static int find_region(float lat, float lon, int level)
{
    for (uint32_t i = 0; i < s_b.nreg; i++)
        if (s_b.reg[i].level == level && in_region(&s_b.reg[i], lat, lon)) return (int)i;
    return -1;
}

static void draw_ring(const ring_t *g, uint8_t attr, uint8_t prio)
{
    int px = 0, py = 0; bool have = false;
    for (uint32_t i = 0; i < g->n; i++) {
        const int16_t x = s_b.proj[2 * (g->p0 + i)], y = s_b.proj[2 * (g->p0 + i) + 1];
        if (x == INT16_MIN) { have = false; continue; }
        if (have && (x != px || y != py)) ls_ink_line(px, py, x, y, attr, prio, 0);
        px = x; py = y; have = true;
    }
}

void ls_globe_draw(tui_surface *sf, tui_rect a, ls_globe_fill_fn fill, void *ctx, const char *selected)
{
    ls_globe_load(NULL);
    s_area = a;
    if (a.w < 8 || a.h < 4) return;
    ease();
    const int wd = a.w * 2, hd = a.h * 3;
    float r = wd / 2.0f, rv = hd / 2.0f / DOT_ASPECT;
    s_R = (r < rv ? r : rv) * 0.94f * s_zoom;
    s_cx = wd / 2.0f; s_cy = hd / 2.0f;
    const center_t c = center_now();

    const bool moved = !s_proj_ok || s_plat != s_lat || s_plon != s_lon || s_pzoom != s_zoom ||
                       s_pw != a.w || s_ph != a.h;
    if (moved && s_b.nreg) {
        for (uint32_t i = 0; i < s_b.npt; i++) {
            const trig_t *t = &s_b.trig[i];
            float x, y;
            if (project(&c, t->sl, t->cl, t->sx, t->cx, &x, &y) &&
                x > -2000 && x < 2000 && y > -2000 && y < 2000) {
                s_b.proj[2 * i] = (int16_t)lrintf(x); s_b.proj[2 * i + 1] = (int16_t)lrintf(y);
            } else s_b.proj[2 * i] = INT16_MIN;
        }
        s_plat = s_lat; s_plon = s_lon; s_pzoom = s_zoom; s_pw = a.w; s_ph = a.h;
        s_proj_ok = true;
        ls_globe_refill();
    }

    /* Fills: once the view is still, a few rows a frame. */
    if (!s_cellreg) s_cellreg = ps(MAX_CELLS * sizeof(uint16_t));
    if (s_cellreg && a.w * a.h <= MAX_CELLS && s_b.nreg) {
        if (s_fill_w != a.w || s_fill_h != a.h) { s_fill_w = a.w; s_fill_h = a.h; s_fill_row = 0; }
        if (!ls_globe_moving())
            for (int n = 0; n < FILL_ROWS && s_fill_row < a.h; n++, s_fill_row++)
                for (int col = 0; col < a.w; col++) {
                    float lat, lon; int reg = -1;
                    if (unproject(col * 2 + 1.0f, s_fill_row * 3 + 1.5f, &lat, &lon))
                        reg = find_region(lat, lon, 1);
                    s_cellreg[s_fill_row * a.w + col] = (uint16_t)(reg + 1);
                }
    }

    if (!ls_ink_begin(a)) return;
    /* the limb and a graticule, faint */
    ls_ink_ellipse((int)s_cx, (int)s_cy, (int)s_R, (int)(s_R * DOT_ASPECT), LS_FAINT_FG, 1, -3);
    for (int lo = -180; lo < 180; lo += GRID_STEP)
        for (int la = -80; la <= 80; la += 4) {
            const float fl = la * (float)M_PI / 180.0f, fo = lo * (float)M_PI / 180.0f;
            float x, y;
            if (project(&c, sinf(fl), cosf(fl), sinf(fo), cosf(fo), &x, &y)) ls_ink_dot((int)x, (int)y, LS_FAINT_FG, 1);
        }
    for (int la = -60; la <= 60; la += GRID_STEP)
        for (int lo = -180; lo < 180; lo += 4) {
            const float fl = la * (float)M_PI / 180.0f, fo = lo * (float)M_PI / 180.0f;
            float x, y;
            if (project(&c, sinf(fl), cosf(fl), sinf(fo), cosf(fo), &x, &y)) ls_ink_dot((int)x, (int)y, LS_FAINT_FG, 1);
        }
    /* Fills: a dot texture in the state's colour, denser for what is on
       the card, white for the selected state; borders draw over them. */
    if (s_cellreg && fill && a.w * a.h <= MAX_CELLS)
        for (int row = 0; row < s_fill_row && row < a.h; row++)
            for (int col = 0; col < a.w; col++) {
                const int reg = s_cellreg[row * a.w + col] - 1;
                if (reg < 0) continue;
                const uint8_t hue = fill(s_b.reg[reg].code, ctx);
                if (!hue) continue;
                const bool sel = selected && !strcmp(selected, s_b.reg[reg].code);
                const uint8_t at = TUI_ATTR(sel ? (TUI_WHITE | TUI_BRIGHT) : (hue | TUI_BRIGHT), TUI_BLACK);
                const int dense = sel || hue == TUI_GREEN ? 4 : hue == TUI_YELLOW ? 3 : 2;
                static const int8_t DX[4] = {0, 1, 1, 0}, DY[4] = {0, 1, 2, 2};
                for (int k = 0; k < dense; k++)
                    ls_ink_dot(col * 2 + DX[(k + row) & 3], row * 3 + DY[k], at, sel ? 4 : 1);
            }
    if (s_b.nreg) {
        const uint8_t country = TUI_ATTR(TUI_CYAN, TUI_BLACK);
        const uint8_t state = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
        const uint8_t pick = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        int sel = -1;
        for (uint32_t i = 0; i < s_b.nreg; i++) {
            const region_t *rg = &s_b.reg[i];
            if (selected && !strcmp(selected, rg->code)) sel = (int)i;
            /* States only once they are big enough to read. */
            if (rg->level == 1 && s_zoom < 1.8f) continue;
            for (uint32_t k = 0; k < rg->nring; k++)
                draw_ring(&s_b.ring[rg->ring0 + k], rg->level ? state : country, rg->level ? 3 : 2);
        }
        if (sel >= 0)
            for (uint32_t k = 0; k < s_b.reg[sel].nring; k++)
                draw_ring(&s_b.ring[s_b.reg[sel].ring0 + k], pick, 5);
    }
    ls_ink_flush(sf, tui_rect_make(0, -1, 0, 0));
}

int ls_globe_hit(int col, int row, char code[8], float *plat, float *plon)
{
    if (!tui_rect_contains(s_area, col, row) || s_R <= 0) return -1;
    float lat, lon;
    if (!unproject((col - s_area.x) * 2 + 1.0f, (row - s_area.y) * 3 + 1.5f, &lat, &lon)) return -1;
    if (plat) *plat = lat;
    if (plon) *plon = lon;
    for (int level = 1; level >= 0; level--) {
        const int r = find_region(lat, lon, level);
        if (r >= 0) { if (code) snprintf(code, 8, "%s", s_b.reg[r].code); return level; }
    }
    return -1;
}

bool ls_globe_region(const char *code, char *name, int cap, float *lat, float *lon)
{
    for (uint32_t i = 0; i < s_b.nreg; i++) {
        const region_t *r = &s_b.reg[i];
        if (strcmp(r->code, code)) continue;
        if (name) snprintf(name, (size_t)cap, "%s", r->name);
        if (lat) *lat = r->clat;
        if (lon) *lon = r->clon;
        return true;
    }
    return false;
}
