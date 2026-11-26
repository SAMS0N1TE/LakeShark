/* WARDRIVE's ATLAS page: the routes and the memory (wd_page.h, wd_atlas.h).

   Drawn on plates (ls_plate.h): heavy frames, the slate ground, one
   headline a plate.

   ROUTES   with two or more routes, a big key a route to pick it.
   ROUTE    the chosen route's shape, large, in braille from the cells its
            runs drove (no two routes look alike), brighter where more runs
            went; this run's cells lit as they are driven, the car at the
            head. Beside it the run in big letters and the devices heard on
            each run; under it THIS RUN against the BEST run.
   COVERAGE one thick meter a source (Wi-Fi, BT, RF, AIR, LORA, CELL): how
            much of the road has been heard with it, the part this run
            heard bright. NOW: the road unrolled, what this run drove, the
            car where it is.
   LISTS    NEW here, regulars MISSING, MOVED, REGULARS heard.
   IMPORT   old drives on the card into the atlas; COMPACT the index now.

   Keys: < > (or left/right) pick a route, TAB or L the list, up/down
   scroll it, I import, C compact, F follow the drive. Everything moves only
   on real data: a cell driven, a number changing, an import reading. */
#include "wd_page.h"
#include "wd_atlas.h"
#include "wd_feed.h"
#include "../ls_tui.h"
#include "../ls_tui_ui.h"
#include "../ls_braille.h"
#include "../ls_plate.h"
#include "../ls_calls.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

/* everything inside a plate on its ground (ls_plate.h) */
#define INK(c)  ls_plate_at((uint8_t)(c))
#define LIT(c)  INK((c) | TUI_BRIGHT)
#define DIM     ls_plate_at(TUI_WHITE)
#define FAINT   ls_plate_at(TUI_BLUE)
#define TEXT    ls_plate_at(TUI_WHITE | TUI_BRIGHT)
#define MAX_RC 512
#define MAX_RUN 1536
#define CV_W 48
#define CV_H 16

static const uint8_t SRC_HUE[WD_AS_N] = { TUI_GREEN, TUI_MAGENTA, TUI_YELLOW, TUI_CYAN, TUI_RED, TUI_BLUE };
static const uint8_t ROUTE_HUE[6] = { TUI_CYAN, TUI_YELLOW, TUI_MAGENTA, TUI_GREEN, TUI_BLUE, TUI_RED };

static EXT_RAM_BSS_ATTR wd_atlas_cell_t s_rc[MAX_RC];
static EXT_RAM_BSS_ATTR wd_atlas_cell_t s_run[MAX_RUN];
static EXT_RAM_BSS_ATTR wd_atlas_item_t s_items[40];
static wd_atlas_info_t s_in;
static int s_sel = -1;              /* the route picked; -1 follows the drive */
static int s_list, s_top;
static uint32_t s_count_ms;
/* numbers that just changed glow for a moment: the change is the motion */
enum { V_DEV, V_NEW, V_REG, V_MISS, V_MOVED, V_CELLS, V_GAIN, V_N };
static unsigned s_val[V_N];
static uint32_t s_val_ms[V_N];
/* touch targets */
#define CARDS_MAX 8
static tui_rect s_card[CARDS_MAX], s_tab[WD_ATLAS_LISTS], s_btn_imp, s_btn_cmp, s_list_r;
static int s_card_route[CARDS_MAX], s_ncard;

static void put(tui_surface *sf, tui_rect clip, int x, int y, const char *s, uint8_t at)
{
    if (y >= clip.y && y < clip.y + clip.h) tui_put_str(sf, clip, x, y, s, at);
}
static void putc_(tui_surface *sf, tui_rect clip, int x, int y, char ch, uint8_t at) { tui_put_char(sf, clip, x, y, ch, at); }
static bool in(tui_rect r, int x, int y) { return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
/* off the plates, on the black */
static uint8_t a_text(void) { return ls_ink(LS_INK_TEXT, TUI_ATTR(TUI_WHITE, TUI_BLACK)); }
static uint8_t a_strong(void) { return ls_ink(LS_INK_STRONG, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)); }

/* ------------------------------------------------------------ a braille canvas */

static EXT_RAM_BSS_ATTR struct { int w, h; uint8_t bits[CV_W * CV_H], hue[CV_W * CV_H], pri[CV_W * CV_H]; } cv;
static void cv_clear(int w, int h)
{
    cv.w = w > CV_W ? CV_W : w;
    cv.h = h > CV_H ? CV_H : h;
    memset(cv.bits, 0, sizeof(cv.bits));
    memset(cv.pri, 0, sizeof(cv.pri));
}
static void cv_dot(int x, int y, uint8_t hue, uint8_t pri)
{
    if (x < 0 || y < 0 || x >= cv.w * 2 || y >= cv.h * 4) return;
    static const uint8_t bit[4][2] = { { 0, 3 }, { 1, 4 }, { 2, 5 }, { 6, 7 } };
    const int c = (y / 4) * cv.w + x / 2;
    cv.bits[c] |= (uint8_t)(1u << bit[y & 3][x & 1]);
    if (pri >= cv.pri[c]) { cv.pri[c] = pri; cv.hue[c] = hue; }
}
static void cv_line(int x0, int y0, int x1, int y1, uint8_t hue, uint8_t pri)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int k = 0; k < 400; k++) {
        cv_dot(x0, y0, hue, pri);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void cv_draw(tui_surface *sf, tui_rect clip, int x, int y)
{
    for (int r = 0; r < cv.h; r++)
        for (int c = 0; c < cv.w; c++) {
            const int i = r * cv.w + c;
            if (cv.bits[i]) ls_braille_put(sf, clip, x + c, y + r, (int16_t)(0x2800 | cv.bits[i]), cv.hue[i]);
        }
}

/* Where cells land on the canvas: the box round `pts`, fitted with east-west
   squeezed by the latitude, so a route keeps its real shape. */
typedef struct { int32_t x0, y0; float sx, sy, ox, oy; } fit_t;
static fit_t fit_cells(const wd_atlas_cell_t *c, int n, const wd_atlas_cell_t *c2, int n2, int dw, int dh)
{
    fit_t f = { 0, 0, 1, 1, 0, 0 };
    int32_t x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;
    for (int k = 0; k < 2; k++) {
        const wd_atlas_cell_t *p = k ? c2 : c;
        for (int i = 0; i < (k ? n2 : n); i++) {
            if (p[i].cx < x0) x0 = p[i].cx;
            if (p[i].cx > x1) x1 = p[i].cx;
            if (p[i].cy < y0) y0 = p[i].cy;
            if (p[i].cy > y1) y1 = p[i].cy;
        }
    }
    if (x0 > x1) return f;
    const float lat = (float)((y0 + y1) / 2) / 640.0f;
    const float kx = cosf(lat * 0.0174533f);
    const float w = (float)(x1 - x0 + 1) * kx, h = (float)(y1 - y0 + 1);
    const float s = fminf((float)(dw - 2) / w, (float)(dh - 2) / h);
    f.x0 = x0; f.y0 = y1;
    f.sx = s * kx; f.sy = s;
    f.ox = ((float)dw - w * s) / 2;
    f.oy = ((float)dh - h * s) / 2;
    return f;
}
static void fit_xy(const fit_t *f, int32_t cx, int32_t cy, int *x, int *y)
{
    *x = (int)(f->ox + ((float)(cx - f->x0) + 0.5f) * f->sx);
    *y = (int)(f->oy + ((float)(f->y0 - cy) + 0.5f) * f->sy);
}
static bool adjacent(const wd_atlas_cell_t *a, const wd_atlas_cell_t *b)
{
    return a->cx - b->cx <= 2 && b->cx - a->cx <= 2 && a->cy - b->cy <= 2 && b->cy - a->cy <= 2;
}
/* A route as a line: consecutive cells joined when they touch. */
static void shape(const wd_atlas_cell_t *c, int n, const fit_t *f, uint8_t hue, uint16_t runs, bool by_hits)
{
    for (int i = 0; i < n; i++) {
        int x, y;
        fit_xy(f, c[i].cx, c[i].cy, &x, &y);
        /* the cells most runs drove are the bright line, a detour stays faint */
        const bool core = !by_hits || c[i].hits * 3 >= runs;
        const uint8_t h = core ? (uint8_t)(hue | TUI_BRIGHT) : hue;
        if (i && adjacent(&c[i - 1], &c[i])) {
            int px, py;
            fit_xy(f, c[i - 1].cx, c[i - 1].cy, &px, &py);
            cv_line(px, py, x, y, INK(h), core ? 2 : 1);
        } else cv_dot(x, y, INK(h), core ? 2 : 1);
    }
}

/* ------------------------------------------------------------ pieces */

static void glow_note(int v, unsigned now_val, uint32_t now)
{
    if (s_val[v] != now_val) { s_val[v] = now_val; s_val_ms[v] = now ? now : 1; }
}
static uint8_t glow(int v, uint32_t now, uint8_t base)
{
    return v >= 0 && s_val_ms[v] && now - s_val_ms[v] < 1500 ? LIT(TUI_YELLOW) : base;
}

/* The headline: big letters when they fit, a bright line when not. */
static void big(tui_surface *sf, tui_rect c, int x, int y, int w, const char *t, uint8_t at)
{
    if (ls_calls_big_width(t) <= w) ls_calls_big_text(sf, c, x, y, t, at);
    else put(sf, c, x, y + 1, t, at);
}

/* ROUTES: a big key a route, the chosen one solid, the page of keys that
   holds it. Only with two or more routes: one route needs no picking. */
static void picker(tui_surface *sf, tui_rect r, int cur)
{
    const int per = r.w / 11 < CARDS_MAX ? r.w / 11 : CARDS_MAX;
    if (per < 2) return;
    const int first = cur >= per ? (cur / per) * per : 0;
    const int n = s_in.routes - first < per ? s_in.routes - first : per;
    const int gap = 1, bw = (r.w - gap * (n - 1)) / n;
    for (int k = 0; k < n; k++) {
        const int i = first + k;
        wd_atlas_route_t rt;
        if (!wd_atlas_route_info(i, &rt)) continue;
        char t[24];
        snprintf(t, sizeof(t), "%s%s", s_in.live && s_in.route == i ? "> " : "", rt.name);
        if ((int)strlen(t) > bw - 2) t[bw - 2 > 0 ? bw - 2 : 0] = 0;
        const tui_rect b = tui_rect_make(r.x + k * (bw + gap), r.y, k == n - 1 ? r.x + r.w - (r.x + k * (bw + gap)) : bw, r.h);
        ls_plate_key(sf, r, b, t, ROUTE_HUE[i % 6], i == cur, true);
        s_card[s_ncard] = b;
        s_card_route[s_ncard++] = i;
    }
}

/* Devices heard on every run of the route, one solid column a run (half-cell
   steps), the best bright, this run (live) yellow at the end. */
static void history(tui_surface *sf, tui_rect c, int x, int y, int w, int rows, int cur, uint8_t hue)
{
    static wd_atlas_run_t runs[32];
    if (rows < 2 || w < 12) return;
    const bool live = s_in.live && s_in.route == cur;
    const int maxn = (w - 5) / 2 - (live ? 1 : 0);
    const int n = wd_atlas_route_runs(cur, runs, maxn < 32 ? maxn : 32);
    if (!n && !live) return;
    const int bars = rows - 1, levels = bars * 2;
    unsigned top = live ? s_in.run_dev : 1, best = 0;
    for (int j = 0; j < n; j++) { if (runs[j].ndev > top) top = runs[j].ndev; if (runs[j].ndev > best) best = runs[j].ndev; }
    if (!top) top = 1;
    for (int j = 0; j < n + (live ? 1 : 0); j++) {
        const bool now_col = j == n;
        const unsigned v = now_col ? s_in.run_dev : runs[j].ndev;
        const int h = v ? 1 + (int)((v * (unsigned)(levels - 1) + top - 1) / top) : 0;
        const uint8_t at = now_col ? LIT(TUI_YELLOW) : v == best ? LIT(hue) : INK(hue);
        const int cx = x + j * 2;
        for (int b = 0; b < bars; b++) {
            const int above = h - b * 2, yy = y + bars - 1 - b;
            if (above >= 1) ls_plate_col(sf, c, cx, yy, above, at);
            else if (!b) putc_(sf, c, cx, yy, '_', FAINT);
        }
    }
    char t[16];
    snprintf(t, sizeof(t), "%u", top);
    put(sf, c, x + w - (int)strlen(t), y, t, TEXT);
    put(sf, c, x, y + bars, "devices a run", DIM);
}

/* One THIS/BEST row: the figure bright, the best dim, and how far this run
   is towards the best as a thick meter. */
static void row_cmp(tui_surface *sf, tui_rect c, int x, int y, int w, const char *label, double a, double b, bool km, int v,
                    uint32_t now, uint8_t hue)
{
    char t[24];
    put(sf, c, x, y, label, DIM);
    if (km) snprintf(t, sizeof(t), "%5.1f", a);
    else snprintf(t, sizeof(t), "%5u", (unsigned)a);
    put(sf, c, x + 9, y, t, glow(v, now, LIT(hue)));
    if (b < 0) snprintf(t, sizeof(t), "%5s", "--");
    else if (km) snprintf(t, sizeof(t), "%5.1f", b);
    else snprintf(t, sizeof(t), "%5u", (unsigned)b);
    put(sf, c, x + 15, y, t, DIM);
    const int mw = w - 22;
    if (mw >= 4 && b > 0) ls_plate_meter(sf, c, x + 22, y, mw, (float)(a / b), hue);
}

/* ROUTE: the shape and the run, then THIS against BEST. */
static void route_plate(tui_surface *sf, tui_rect r, int cur, uint32_t now)
{
    wd_atlas_route_t rt;
    const bool have = cur >= 0 && wd_atlas_route_info(cur, &rt);
    const bool live = s_in.live, here = live && have && s_in.route == cur;
    char title[32], chip[16] = "";
    uint8_t chip_hue = TUI_WHITE;
    snprintf(title, sizeof(title), "%s", have ? rt.name : live ? "THIS DRIVE" : "ROUTE");
    if (here) { snprintf(chip, sizeof(chip), "LIVE"); chip_hue = TUI_GREEN; }
    else if (live && !have) { snprintf(chip, sizeof(chip), "%s", s_in.run_cells >= 6 ? "NEW ROAD" : "FINDING"); chip_hue = TUI_YELLOW; }
    const uint8_t hue = have ? ROUTE_HUE[cur % 6] : TUI_WHITE;
    const tui_rect c = ls_plate(sf, r, r, hue, title, chip, chip_hue);
    if (c.w < 30 || c.h < 8) return;
    /* top: the shape at the left, the headline at the right; the table under
       it gets a blank row between its figures when there is room */
    const int rw = 22, sw = c.w - rw - 1;
    const bool spaced = c.h >= 16;
    const int tbl = spaced ? 11 : 7;
    const int sh = c.h - tbl > 8 ? 8 : c.h - tbl < 3 ? 3 : c.h - tbl;
    const int y = c.y + 1, rx = c.x + sw + 1;
    const int n = have ? wd_atlas_route_cells(cur, s_rc, MAX_RC) : 0;
    const int nr = live ? wd_atlas_run_cells(s_run, MAX_RUN) : 0;
    if (n || nr) {
        cv_clear(sw, sh);
        const fit_t f = fit_cells(s_rc, n, s_run, nr, cv.w * 2, cv.h * 4);
        if (n) shape(s_rc, n, &f, hue, have ? rt.runs : 1, true);
        for (int i = 0; i < nr; i++) {
            int x, yy;
            fit_xy(&f, s_run[i].cx, s_run[i].cy, &x, &yy);
            if (i && adjacent(&s_run[i - 1], &s_run[i])) {
                int px, py;
                fit_xy(&f, s_run[i - 1].cx, s_run[i - 1].cy, &px, &py);
                cv_line(px, py, x, yy, LIT(TUI_GREEN), 3);
            } else cv_dot(x, yy, LIT(TUI_GREEN), 3);
        }
        if (nr) {
            int x, yy;
            fit_xy(&f, s_run[nr - 1].cx, s_run[nr - 1].cy, &x, &yy);
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) cv_dot(x + dx, yy + dy, LIT(TUI_YELLOW), 4);
        }
        cv_draw(sf, c, c.x, y);
    } else {
        put(sf, c, c.x, y + sh / 2 - 1, "no cells yet", TEXT);
        put(sf, c, c.x, y + sh / 2, "waiting for a GPS fix", DIM);
    }
    char t[32];
    if (here) {
        snprintf(t, sizeof(t), "RUN %u", (unsigned)s_in.run_num);
        big(sf, c, rx, y, rw, t, LIT(hue));
    } else if (live && !have) {
        big(sf, c, rx, y, rw, "NEW", LIT(TUI_YELLOW));
        put(sf, c, rx, y + 4, s_in.run_cells >= 6 ? "a road not in" : "finding the road", TEXT);
        if (s_in.run_cells >= 6) put(sf, c, rx, y + 5, "the atlas yet", TEXT);
    } else if (have) {
        snprintf(t, sizeof(t), "%u RUN%s", (unsigned)rt.runs, rt.runs == 1 ? "" : "S");
        big(sf, c, rx, y, rw, t, LIT(hue));
    }
    if (have) history(sf, c, rx, y + 4, rw, sh - 4, cur, hue);
    /* under it: THIS (or LAST) against BEST, one row a figure */
    int ty = y + sh + 1;
    const int step = spaced ? 2 : 1;
    if (ty + 3 * step + 1 >= c.y + c.h) return;
    const wd_atlas_run_t *last = have ? &rt.last : NULL, *best = have ? &rt.best : NULL;
    put(sf, c, c.x + 10, ty, live ? "THIS" : "LAST", TEXT);
    put(sf, c, c.x + 16, ty, "BEST", DIM);
    ty++;
    const double dev = live ? s_in.run_dev : last ? last->ndev : 0;
    const double nw = live ? s_in.run_new : last ? last->nnew : 0;
    const double cells = live ? s_in.run_cells : last ? last->ncells : 0;
    const double km = live ? s_in.run_m / 1000.0 : last ? last->dist_m / 1000.0 : 0;
    row_cmp(sf, c, c.x, ty, c.w, "DEVICES", dev, best ? best->ndev : -1, false, live ? V_DEV : -1, now, TUI_CYAN);
    ty += step;
    row_cmp(sf, c, c.x, ty, c.w, "NEW HERE", nw, best ? best->nnew : -1, false, live ? V_NEW : -1, now, TUI_YELLOW);
    ty += step;
    row_cmp(sf, c, c.x, ty, c.w, "CELLS", cells, best ? best->ncells : -1, false, live ? V_CELLS : -1, now, TUI_WHITE);
    ty += step;
    row_cmp(sf, c, c.x, ty, c.w, "KM", km, best ? best->dist_m / 1000.0 : -1, true, -1, now, TUI_GREEN);
}

/* COVERAGE: a thick meter a source, how much of the road has been heard with
   it (the part this run heard bright, earlier runs solid, never the track);
   then NOW, the road unrolled: driven solid, the car where it is. */
static void coverage_plate(tui_surface *sf, tui_rect r, int cur)
{
    wd_atlas_route_t rt;
    const bool have = cur >= 0 && wd_atlas_route_info(cur, &rt);
    const int n = have ? wd_atlas_route_cells(cur, s_rc, MAX_RC) : s_in.live ? wd_atlas_run_cells(s_rc, MAX_RC) : 0;
    char chip[16] = "";
    if (n) snprintf(chip, sizeof(chip), "%d CELLS", n);
    const tui_rect c = ls_plate(sf, r, r, TUI_GREEN, "COVERAGE", chip, TUI_WHITE);
    if (c.w < 20 || c.h < 2) return;
    if (!n) { put(sf, c, c.x, c.y + 1, "drive a route to fill this in", DIM); return; }
    int any[WD_AS_N] = { 0 }, cur_n[WD_AS_N] = { 0 }, ns = 0, driven = 0;
    for (int k = 0; k < n; k++) {
        if (s_rc[k].now) driven++;
        for (int s = 0; s < WD_AS_N; s++) {
            const bool nw = (s_rc[k].now & (1u << s)) != 0;
            if (s_rc[k].passes[s] > 0 || nw) any[s]++;
            if (nw) cur_n[s]++;
        }
    }
    for (int s = 0; s < WD_AS_N; s++) ns += any[s] || (s_in.live && (s_in.run_src & (1u << s)));
    /* two sources a row, a blank row between rows when there is room */
    const bool now_row = s_in.live;
    const int rows = (ns + 1) / 2;
    const int tight = rows + (now_row ? 1 : 0);
    const bool air = c.h >= tight + 1, gaps = c.h >= tight + (air ? 1 : 0) + (rows - 1) + (now_row ? 1 : 0);
    const int step = gaps ? 2 : 1, y0 = c.y + (air ? 1 : 0);
    const int colw = (c.w - 2) / 2, lw = 6, pw = 5, cmw = colw - lw - pw;
    char t[16];
    int idx = 0;
    for (int s = 0; s < WD_AS_N && cmw >= 3; s++) {
        const bool listening = s_in.live && (s_in.run_src & (1u << s));
        if (!any[s] && !listening) continue;
        const int x = c.x + (idx % 2) * (colw + 2), y = y0 + (idx / 2) * step;
        idx++;
        if (y >= c.y + c.h - (now_row ? 1 : 0)) break;
        put(sf, c, x, y, wd_atlas_src_name(s), LIT(SRC_HUE[s]));
        const float fa = (float)any[s] / (float)n, fn = (float)cur_n[s] / (float)n;
        ls_plate_meter(sf, c, x + lw, y, cmw, fa, SRC_HUE[s]);
        if (s_in.live) {
            /* earlier runs only: solid but not lit, so this run's part stands out */
            const int lit = (int)(fn * (float)cmw + 0.5f), all = (int)(fa * (float)cmw + 0.5f);
            for (int i = lit; i < all; i++) putc_(sf, c, x + lw + i, y, '=', INK(SRC_HUE[s]));
        }
        snprintf(t, sizeof(t), "%3d%%", any[s] * 100 / n);
        put(sf, c, x + colw - 4, y, t, any[s] ? TEXT : DIM);
    }
    if (!now_row) return;
    const int mw = c.w - lw - pw;
    const int y = y0 + rows * step;
    if (y >= c.y + c.h) return;
    put(sf, c, c.x, y, "NOW", LIT(TUI_YELLOW));
    int head = -1;
    for (int i = 0; i < mw; i++) {
        const int a = i * n / mw, b = (i + 1) * n / mw > a ? (i + 1) * n / mw : a + 1;
        bool d = false;
        for (int k = a; k < b && k < n; k++) if (s_rc[k].now) d = true;
        if (d) head = i;
        putc_(sf, c, c.x + lw + i, y, d ? '=' : '.', d ? LIT(TUI_GREEN) : INK(TUI_BLUE));
    }
    if (head >= 0) {
        /* the car: the last cell this run drove, wherever it sits on the road */
        const int nr = wd_atlas_run_cells(s_run, MAX_RUN);
        if (nr) {
            const wd_atlas_cell_t *h = &s_run[nr - 1];
            for (int k = 0; k < n; k++)
                if (s_rc[k].cx == h->cx && s_rc[k].cy == h->cy) { head = k * mw / n; break; }
        }
        putc_(sf, c, c.x + lw + head, y, '>', TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT));
    }
    snprintf(t, sizeof(t), "%3d%%", driven * 100 / n);
    put(sf, c, c.x + c.w - 4, y, t, TEXT);
}

static const char *const LIST_NAME[WD_ATLAS_LISTS] = { "NEW", "MISSING", "MOVED", "REGULAR" };
static const char *const LIST_SHORT[WD_ATLAS_LISTS] = { "NEW", "MISS", "MOVED", "REG" };
static const uint8_t LIST_HUE[WD_ATLAS_LISTS] = { TUI_YELLOW, TUI_RED, TUI_MAGENTA, TUI_GREEN };

/* The lists: a tab a list (solid when chosen), then the list. */
static void lists_plate(tui_surface *sf, tui_rect r)
{
    const tui_rect c = ls_plate(sf, r, r, LIST_HUE[s_list], s_in.live ? "THIS RUN" : s_in.recent ? "LAST DRIVE" : "EACH RUN", NULL, 0);
    if (c.w < 20 || c.h < 3) return;
    const int counts[WD_ATLAS_LISTS] = { (int)s_in.run_new, (int)s_in.run_missing, (int)s_in.run_moved, (int)s_in.run_reg };
    const int kh = c.h >= 8 ? 3 : 1, gap = 1, ty = c.y + (c.h >= 9 ? 1 : 0);
    const int tw = (c.w - gap * (WD_ATLAS_LISTS - 1)) / WD_ATLAS_LISTS;
    for (int k = 0; k < WD_ATLAS_LISTS; k++) {
        char t[24];
        const int x = c.x + k * (tw + gap), w = k == WD_ATLAS_LISTS - 1 ? c.x + c.w - x : tw, room = w - (kh >= 3 ? 2 : 0);
        snprintf(t, sizeof(t), "%s %d", LIST_NAME[k], counts[k]);
        if ((int)strlen(t) > room) snprintf(t, sizeof(t), "%s %d", LIST_SHORT[k], counts[k]);
        if ((int)strlen(t) > room) snprintf(t, sizeof(t), "%s", LIST_SHORT[k]);
        const tui_rect b = tui_rect_make(x, ty, w, kh);
        ls_plate_key(sf, c, b, t, LIST_HUE[k], k == s_list, true);
        s_tab[k] = kh >= 3 ? b : tui_rect_make(b.x, b.y, b.w, 2);
    }
    const int ly = ty + kh + 1;
    s_list_r = tui_rect_make(c.x, ly, c.w, c.y + c.h - ly);
    if (!s_in.live && !s_in.recent) {
        put(sf, c, c.x, ly, "START a drive: what is new here,", TEXT);
        put(sf, c, c.x, ly + 1, "which regulars are missing and", TEXT);
        put(sf, c, c.x, ly + 2, "what moved show up here.", TEXT);
        return;
    }
    const int n = wd_atlas_list(s_list, s_items, (int)(sizeof(s_items) / sizeof(s_items[0])));
    const int rows = c.y + c.h - ly;
    if (s_top > n - rows) s_top = n - rows;
    if (s_top < 0) s_top = 0;
    if (!n) {
        static const char *const EMPTY[WD_ATLAS_LISTS] = { "nothing new here yet", "no regulars missing", "nothing moved",
                                                           "no regulars heard yet" };
        put(sf, c, c.x, ly, EMPTY[s_list], TEXT);
        if (!s_in.known_ready) put(sf, c, c.x, ly + 1, "(reading what the atlas knows here)", DIM);
        return;
    }
    for (int i = 0; i < rows && s_top + i < n; i++) {
        const wd_atlas_item_t *it = &s_items[s_top + i];
        const int y = ly + i;
        char tag[8], right[16], line[64], nm[24];
        snprintf(tag, sizeof(tag), "%-5s", wd_kind_short(it->kind));
        if (s_list == WD_ATLAS_L_MOVED) snprintf(right, sizeof(right), "%.1f km", it->dist_m / 1000.0);
        else if (s_list == WD_ATLAS_L_NEW) snprintf(right, sizeof(right), "%d dBm", it->rssi);
        else snprintf(right, sizeof(right), "%u drives", it->drives);
        put(sf, c, c.x, y, tag, LIT(SRC_HUE[wd_atlas_src(it->kind)]));
        if (it->name[0]) snprintf(nm, sizeof(nm), "%s", it->name);
        else snprintf(nm, sizeof(nm), "%02X:%02X:%02X:%02X:%02X:%02X", it->addr[0], it->addr[1], it->addr[2], it->addr[3], it->addr[4],
                      it->addr[5]);
        const int nw = c.w - 6 - (int)strlen(right) - 1;
        snprintf(line, sizeof(line), "%-*.*s", nw > 0 ? nw : 0, nw > 0 ? nw : 0, nm);
        put(sf, c, c.x + 6, y, line, TEXT);
        put(sf, c, c.x + c.w - (int)strlen(right), y, right, LIT(LIST_HUE[s_list]));
    }
}

/* IMPORT and COMPACT as big keys, and what the atlas is doing beside them. */
static void actions(tui_surface *sf, tui_rect r)
{
    const bool busy = s_in.job != WD_ATLAS_JOB_NONE;
    /* a narrow tall column stacks the keys; otherwise side by side */
    const bool stack = r.w < 30 && r.h >= 9;
    const int bw = stack ? r.w : r.w >= 60 ? 18 : 15, kh = 3;
    char t[64];
    if (s_in.import_left > 0) snprintf(t, sizeof(t), "IMPORT %d", s_in.import_left);
    else snprintf(t, sizeof(t), "IMPORT");
    s_btn_imp = tui_rect_make(r.x, r.y, bw, stack ? kh : r.h);
    ls_plate_key(sf, r, s_btn_imp, t, TUI_CYAN, false, !(busy || s_in.live || s_in.import_left == 0));
    if (!s_in.live && s_in.tail) snprintf(t, sizeof(t), "COMPACT %u", (unsigned)s_in.tail);
    else snprintf(t, sizeof(t), "COMPACT");
    s_btn_cmp = stack ? tui_rect_make(r.x, r.y + kh + 1, bw, kh) : tui_rect_make(r.x + bw + 1, r.y, bw, r.h);
    ls_plate_key(sf, r, s_btn_cmp, t, TUI_MAGENTA, false, !(busy || s_in.live));
    const tui_rect st = stack ? tui_rect_make(r.x, r.y + 2 * kh + 2, r.w, r.h - 2 * kh - 2)
                              : tui_rect_make(r.x + 2 * bw + 3, r.y, r.w - 2 * bw - 3, r.h);
    if (st.h < 1) return;
    if (st.w < 8) return;
    if (busy) {
        /* the job's own progress: the card being read */
        snprintf(t, sizeof(t), "%s %d%%", s_in.job == WD_ATLAS_JOB_IMPORT ? "IMPORTING" : "COMPACTING", s_in.job_pct);
        put(sf, st, st.x, st.y, t, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        if (st.h >= 2) {
            tui_fill(sf, tui_rect_make(st.x, st.y + 1, st.w, 1), ' ', ls_plate_at(TUI_WHITE));
            ls_plate_meter(sf, st, st.x, st.y + 1, st.w, (float)s_in.job_pct / 100.0f, TUI_CYAN);
        }
        return;
    }
    snprintf(t, sizeof(t), "%.*s", st.w, s_in.note);
    put(sf, st, st.x, st.y, t, a_strong());
    if (s_in.live) snprintf(t, sizeof(t), "after the drive");
    else if (s_in.import_left == 0) snprintf(t, sizeof(t), "all drives in");
    else snprintf(t, sizeof(t), "seq %u", (unsigned)s_in.seq);
    if (st.h >= 2 && (int)strlen(t) <= st.w) put(sf, st, st.x, st.y + 1, t, a_text());
    snprintf(t, sizeof(t), "board %08X", (unsigned)s_in.board);
    if (st.h >= 3 && (int)strlen(t) <= st.w) put(sf, st, st.x, st.y + 2, t, ls_ink(LS_INK_DIM, LS_ATTR_FAINT));
}

/* The page with nothing in it yet: a folded map, and how it fills. */
static void empty_plate(tui_surface *sf, tui_rect r)
{
    static const char *const ART[] = {
        "     ______ ______ ______ ______",
        "    /     //     //     //     /|",
        "   / .   //  ~~ //   . //  .  / |",
        "  /_____//_____//_____//_____/  |",
        "  |  .  ||     ||  ~  ||     | /",
        "  |_____||_____||_____||_____|/",
    };
    static const uint8_t AH[] = { TUI_CYAN, TUI_CYAN, TUI_GREEN, TUI_CYAN, TUI_YELLOW, TUI_CYAN };
    const tui_rect c = ls_plate(sf, r, r, TUI_CYAN, "ATLAS", "EMPTY", TUI_WHITE);
    if (c.w < 34 || c.h < 8) return;
    int y = c.y + 1;
    const int x = c.x + (c.w - 34) / 2;
    for (int i = 0; i < 6; i++) put(sf, c, x, y++, ART[i], INK(AH[i] | (i == 2 || i == 4 ? TUI_BRIGHT : 0)));
    y++;
    if (ls_calls_big_width("RUN 1") <= c.w && y + 3 < c.y + c.h) {
        ls_calls_big_text(sf, c, c.x + (c.w - ls_calls_big_width("RUN 1")) / 2, y, "RUN 1", LIT(TUI_CYAN));
        y += 4;
    }
    static const char *const T[] = {
        "Every drive adds to one map.",
        "",
        "Drive the same road again and it is",
        "recognised: run 2, run 3... each one",
        "fills in what the others missed,",
        "whatever radios that run listened with.",
    };
    for (int i = 0; i < 6 && y < c.y + c.h; i++, y++) put(sf, c, c.x + (c.w - (int)strlen(T[i])) / 2, y, T[i], i ? DIM : TEXT);
}

/* The top row: what the atlas holds over all time. */
static void header(tui_surface *sf, tui_rect a)
{
    char t[24];
    tui_fill(sf, tui_rect_make(a.x, a.y, a.w, 1), ' ', ls_plate_at(TUI_WHITE));
    int x = a.x + ls_plate_tab(sf, a, a.x, a.y, "ATLAS", TUI_CYAN) + 1;
    const struct { unsigned v; const char *w; } F[] = {
        { s_in.devices, "DEV" }, { s_in.runs, "RUNS" }, { (unsigned)s_in.routes, s_in.routes == 1 ? "ROUTE" : "ROUTES" },
        { s_in.km, "KM" }, { s_in.cells, "CELLS" },
    };
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
        snprintf(t, sizeof(t), "%u", F[i].v);
        if (x + (int)strlen(t) + (int)strlen(F[i].w) + 1 > a.x + a.w) break;
        put(sf, a, x, a.y, t, TEXT);
        x += (int)strlen(t) + 1;
        put(sf, a, x, a.y, F[i].w, DIM);
        x += (int)strlen(F[i].w) + 2;
    }
}

/* ------------------------------------------------------------ the page */

static void draw(tui_surface *sf, tui_rect a, uint32_t now)
{
    /* the atlas works in the worker; with none running (the simulator), here */
    if (wd_atlas_pending() && !wd_feed_spawn()) wd_atlas_step(now);
    wd_atlas_info(&s_in);
    if (s_in.state == WD_ATLAS_READY && !s_in.live && (!s_count_ms || now - s_count_ms > 30000)) {
        s_count_ms = now ? now : 1;
        wd_atlas_request(WD_ATLAS_REQ_COUNT);
    }
    s_ncard = 0;
    s_btn_imp = s_btn_cmp = s_list_r = tui_rect_make(0, 0, 0, 0);
    for (int k = 0; k < WD_ATLAS_LISTS; k++) s_tab[k] = tui_rect_make(0, 0, 0, 0);
    if (s_in.state == WD_ATLAS_NOCARD || s_in.state == WD_ATLAS_NOMEM || s_in.state == WD_ATLAS_OFF || s_in.state == WD_ATLAS_LOADING) {
        const char *why = s_in.state == WD_ATLAS_NOCARD ? "No card: the atlas lives on the SD card"
                          : s_in.state == WD_ATLAS_NOMEM ? "Not enough memory for the atlas"
                                                         : "Reading the atlas";
        ls_panel_notice(sf, a, "ATLAS", why, s_in.note[0] ? s_in.note : "routes and memory");
        return;
    }
    if (s_in.live) {
        glow_note(V_DEV, s_in.run_dev, now); glow_note(V_NEW, s_in.run_new, now); glow_note(V_REG, s_in.run_reg, now);
        glow_note(V_MISS, s_in.run_missing, now); glow_note(V_MOVED, s_in.run_moved, now);
        glow_note(V_CELLS, s_in.run_cells, now); glow_note(V_GAIN, s_in.run_gain, now);
    }
    if (s_sel >= s_in.routes) s_sel = s_in.routes - 1;
    const int cur = s_sel >= 0 ? s_sel : s_in.live ? s_in.route : s_in.routes ? 0 : -1;
    header(sf, a);
    const tui_rect body = tui_rect_make(a.x, a.y + 2, a.w, a.h - 2);
    const int act_h = 3;
    if (!s_in.routes && !s_in.live) {
        empty_plate(sf, tui_rect_make(body.x, body.y, body.w, body.h - act_h - 1));
        actions(sf, tui_rect_make(body.x, body.y + body.h - act_h, body.w, act_h));
        return;
    }
    const int pick_h = s_in.routes >= 2 ? 4 : 0;
    if (body.w >= 92) {
        /* landscape: the route and the keys at the left, coverage and the lists at the right */
        const tui_rect left = tui_rect_make(body.x, body.y, 52, body.h);
        tui_rect right = tui_rect_make(body.x + 53, body.y, body.w - 53, body.h);
        tui_rect hole;
        if (ls_tui_cutout(&hole) && hole.x > right.x && hole.x < right.x + right.w) right.w = hole.x - 1 - right.x;
        if (pick_h) picker(sf, tui_rect_make(left.x, left.y, left.w, 3), cur);
        route_plate(sf, tui_rect_make(left.x, left.y + pick_h, left.w, left.h - pick_h), cur, now);
        const int cov_h = right.h >= 20 ? 10 : 8;
        coverage_plate(sf, tui_rect_make(right.x, right.y, right.w, cov_h), cur);
        /* under it the lists, and the keys stacked at their right */
        const int ly = right.y + cov_h + 1, lh = right.y + right.h - ly, kw = 20;
        if (lh >= 5) lists_plate(sf, tui_rect_make(right.x, ly, right.w - kw - 1, lh));
        actions(sf, tui_rect_make(right.x + right.w - kw, ly, kw, lh));
        return;
    }
    /* portrait, top to bottom: routes, the route, coverage, the lists, the keys */
    int y = body.y;
    if (pick_h) { picker(sf, tui_rect_make(body.x, y, body.w, 3), cur); y += pick_h; }
    const int route_h = body.h - pick_h >= 50 ? 20 : 17;
    route_plate(sf, tui_rect_make(body.x, y, body.w, route_h), cur, now);
    y += route_h + 1;
    const int cov_h = body.h - pick_h >= 46 ? 10 : 8;
    coverage_plate(sf, tui_rect_make(body.x, y, body.w, cov_h), cur);
    y += cov_h + 1;
    const int list_h = body.y + body.h - act_h - 1 - y;
    if (list_h >= 5) lists_plate(sf, tui_rect_make(body.x, y, body.w, list_h));
    actions(sf, tui_rect_make(body.x, body.y + body.h - act_h, body.w, act_h));
}

static void pick(int d)
{
    if (!s_in.routes) return;
    int c = s_sel >= 0 ? s_sel : s_in.live && s_in.route >= 0 ? s_in.route : 0;
    c = (c + d + s_in.routes) % s_in.routes;
    s_sel = c;
}

static bool key(ls_tk_t k, char ch, uint32_t now)
{
    (void)now;
    if (k == LS_TK_LEFT || ch == '<' || ch == ',') { pick(-1); return true; }
    if (k == LS_TK_RIGHT || ch == '>' || ch == '.') { pick(+1); return true; }
    if (k == LS_TK_TAB || ch == 'l' || ch == 'L') { s_list = (s_list + 1) % WD_ATLAS_LISTS; s_top = 0; return true; }
    if (k == LS_TK_UP) { if (s_top > 0) s_top--; return true; }
    if (k == LS_TK_DOWN) { s_top++; return true; }
    if (ch == 'i' || ch == 'I') { if (!s_in.live) wd_atlas_request(WD_ATLAS_REQ_IMPORT); return true; }
    if (ch == 'c' || ch == 'C') { if (!s_in.live) wd_atlas_request(WD_ATLAS_REQ_COMPACT); return true; }
    if (ch == 'f' || ch == 'F') { s_sel = -1; return true; }     /* follow the drive again */
    return false;
}

static bool touch(int x, int y, uint32_t now)
{
    (void)now;
    for (int i = 0; i < s_ncard; i++)
        if (in(s_card[i], x, y)) { s_sel = s_card_route[i] == s_sel && s_in.live ? -1 : s_card_route[i]; return true; }
    for (int k = 0; k < WD_ATLAS_LISTS; k++)
        if (in(s_tab[k], x, y)) { s_list = k; s_top = 0; return true; }
    if (in(s_btn_imp, x, y)) { if (!s_in.live) wd_atlas_request(WD_ATLAS_REQ_IMPORT); return true; }
    if (in(s_btn_cmp, x, y)) { if (!s_in.live) wd_atlas_request(WD_ATLAS_REQ_COMPACT); return true; }
    if (in(s_list_r, x, y)) {
        /* the top half scrolls up, the bottom half down */
        if (y < s_list_r.y + s_list_r.h / 2) { if (s_top > 0) s_top -= 3; if (s_top < 0) s_top = 0; }
        else s_top += 3;
        return true;
    }
    return false;
}

const wd_page_t wd_page_atlas = { draw, key, touch, NULL };
