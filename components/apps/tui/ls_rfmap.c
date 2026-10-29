#include "ls_rfmap.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "ls_fox.h"
#include "ls_fox_run.h"
#include "ls_map_ink.h"
#include "ls_picker.h"
#include "ls_rfs.h"
#include "ls_rfs_run.h"
#include "ls_tui_ui.h"
#include "ls_textpack.h"

#define M_PER_DEG_LAT 111132.0

uint8_t ls_rfmap_hue(int step)
{
    static const uint8_t H[5] = { TUI_BLUE | TUI_BRIGHT, TUI_CYAN, TUI_GREEN | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT, TUI_RED | TUI_BRIGHT };
    return H[step < 0 ? 0 : step > 4 ? 4 : step];
}

char ls_rfmap_glyph(int step) { return ".:+*#"[step < 0 ? 0 : step > 4 ? 4 : step]; }

int ls_rfmap_step(float v, float lo, float hi)
{
    if (!(hi > lo)) return 4;
    const int s = (int)floorf((v - lo) / (hi - lo) * 5.0f);
    return s < 0 ? 0 : s > 4 ? 4 : s;
}

/* The colour scale: the data's own span, never less than 12 dB so a quiet
   band is not stretched into false contrast. Busy percent is always 0..100. */
static bool scale(const ls_rfs_grid_t *g, int ch, ls_rfs_metric_t m, float *lo, float *hi)
{
    if (!ls_rfs_grid_range(g, ch, m, lo, hi)) return false;
    if (m == LS_RFS_OCC) { *lo = 0; *hi = 100; return true; }
    if (*hi - *lo < 12) *lo = *hi - 12;
    return true;
}

bool ls_rfmap_has_heat(void)
{
    const ls_rfs_grid_t *g = ls_rfs_grid_lock();
    const bool any = g->count > 0;
    ls_rfs_grid_unlock();
    return any;
}

bool ls_rfmap_has_fox(void) { return ls_fox_count() > 0; }

/* A stipple: the louder, the denser, so heat reads even in one colour. */
static bool stipple(int x, int y, int step)
{
    switch (step) {
    case 0:  return !(x & 3) && !(y % 3);
    case 1:  return !(x & 1) && !(y % 3);
    case 2:  return !(x & 1) && !(y & 1);
    case 3:  return !((x + y) & 1);
    default: return ((x + y) & 3) != 3;
    }
}

void ls_rfmap_heat(const ls_rfmap_frame_t *f)
{
    if (!f || !f->dot) return;
    int ch; ls_rfs_metric_t m;
    ls_rfs_shown(&ch, &m);
    EXT_RAM_BSS_ATTR static double tlat[LS_RFS_TRACK], tlon[LS_RFS_TRACK];
    const ls_rfs_grid_t *g = ls_rfs_grid_lock();
    float lo = 0, hi = 0;
    if (g->count && scale(g, ch, m, &lo, &hi)) {
        const double half_lat = g->cell_m / 2 / M_PER_DEG_LAT, half_lon = g->cell_m / 2 / g->m_per_deg_lon;
        for (int i = 0; i < g->count; i++) {
            float v;
            if (!ls_rfs_grid_value(g, i, ch, m, &v)) continue;
            double clat, clon;
            ls_rfs_grid_centre(g, i, &clat, &clon);
            int x0, y0, x1, y1;
            if (!f->dot(clat + half_lat, clon - half_lon, &x0, &y0) || !f->dot(clat - half_lat, clon + half_lon, &x1, &y1)) continue;
            if (x1 < 0 || y1 < 0 || x0 >= f->w || y0 >= f->h) continue;
            const int step = ls_rfmap_step(v, lo, hi);
            const uint8_t at = TUI_ATTR(ls_rfmap_hue(step), TUI_BLACK);
            if (x1 - x0 < 2 && y1 - y0 < 2) { ls_ink_dot((x0 + x1) / 2, (y0 + y1) / 2, at, 2); continue; }
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > f->w) x1 = f->w;
            if (y1 > f->h) y1 = f->h;
            /* A cell many cells across: a ring of its edge and a stipple
               inside, at most a few hundred dots whatever the zoom. */
            const int stride = (x1 - x0) * (y1 - y0) > 1600 ? 2 : 1;
            for (int y = y0; y < y1; y += stride)
                for (int x = x0; x < x1; x += stride)
                    if (stipple(x / stride, y / stride, step)) ls_ink_dot(x, y, at, 2);
        }
    }
    const int n = ls_rfs_track(tlat, tlon, LS_RFS_TRACK);
    ls_rfs_grid_unlock();
    int px = 0, py = 0; bool have = false;
    const uint8_t trail = TUI_ATTR(TUI_MAGENTA, TUI_BLACK);
    for (int i = 0; i < n; i++) {
        int x, y;
        if (!f->dot(tlat[i], tlon[i], &x, &y)) { have = false; continue; }
        if (have && (px != x || py != y)) ls_ink_line(px, py, x, y, trail, 1, 2);
        px = x; py = y; have = true;
    }
}

void ls_rfmap_fox(const ls_rfmap_frame_t *f)
{
    if (!f || !f->dot) return;
    EXT_RAM_BSS_ATTR static ls_fox_hunt_t h;
    ls_fox_get(&h);
    if (!h.n) return;
    ls_fox_fix_t fix;
    const bool have = ls_fox_fix_now(&fix);
    for (int i = 0; i < h.n; i++) {
        const ls_df_bearing_t *b = &h.b[i];
        double reach = 3000;
        if (have) {
            const double d = hypot((fix.lat - b->lat) * M_PER_DEG_LAT, (fix.lon - b->lon) * M_PER_DEG_LAT * cos(b->lat * M_PI / 180));
            reach = d * 1.6 > 300 ? d * 1.6 : 300;
        }
        double lat2, lon2;
        ls_fox_project(b->lat, b->lon, b->bearing, reach, &lat2, &lon2);
        int x0, y0, x1, y1;
        if (!f->dot(b->lat, b->lon, &x0, &y0) || !f->dot(lat2, lon2, &x1, &y1)) continue;
        const bool newest = i == h.n - 1;
        const uint8_t at = TUI_ATTR(newest ? TUI_YELLOW | TUI_BRIGHT : TUI_YELLOW, TUI_BLACK);
        ls_ink_line(x0, y0, x1, y1, at, newest ? 5 : 4, newest ? 0 : 3);
        /* Where it was taken from: a small cross. */
        ls_ink_line(x0 - 2, y0, x0 + 2, y0, at, 6, 0);
        ls_ink_line(x0, y0 - 2, x0, y0 + 2, at, 6, 0);
    }
    if (!have) return;
    /* Two sigma: about nine fixes in ten land inside. */
    const uint8_t red = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const double a = 2 * fix.major_m, bb = 2 * fix.minor_m, t = fix.major_deg * M_PI / 180;
    const double mlon = M_PER_DEG_LAT * cos(fix.lat * M_PI / 180);
    int fx = 0, fy = 0, px = 0, py = 0;
    bool prev = false;
    for (int k = 0; k <= 48; k++) {
        const double u = k * 2 * M_PI / 48, p = a * cos(u), q = bb * sin(u);
        /* Major axis along bearing t (east sin t, north cos t), minor at t + 90. */
        const double e = p * sin(t) + q * cos(t), n = p * cos(t) - q * sin(t);
        int x, y;
        if (!f->dot(fix.lat + n / M_PER_DEG_LAT, fix.lon + e / mlon, &x, &y)) { prev = false; continue; }
        if (prev) ls_ink_line(px, py, x, y, red, 4, 1);
        px = x; py = y; prev = true;
    }
    if (f->dot(fix.lat, fix.lon, &fx, &fy)) {
        ls_ink_line(fx - 3, fy - 3, fx + 3, fy + 3, red, 8, 0);
        ls_ink_line(fx - 3, fy + 3, fx + 3, fy - 3, red, 8, 0);
    }
}

tui_rect ls_rfmap_legend(tui_surface *sf, tui_rect body, bool heat, bool fox)
{
    tui_rect r = tui_rect_make(body.x, body.y, 0, 0);
    if (!heat && !fox) return r;
    const int w = body.w < 30 ? body.w : 30;
    int row = body.y;
    const uint8_t ink = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK), dim = LS_ATTR_DIM;
    char line[48];
    if (heat) {
        EXT_RAM_BSS_ATTR static ls_rfs_view_t v;
        ls_rfs_view(&v);
        int ch; ls_rfs_metric_t m; ls_rfs_shown(&ch, &m);
        float lo = 0, hi = 0;
        const ls_rfs_grid_t *g = ls_rfs_grid_lock();
        const bool any = scale(g, ch, m, &lo, &hi);
        ls_rfs_grid_unlock();
        for (int x = 0; x < w; x++) { tui_put_char(sf, body, body.x + x, row, ' ', ink); tui_put_char(sf, body, body.x + x, row + 1, ' ', ink); }
        snprintf(line, sizeof(line), "RF HEAT %.8s %s", ch >= 0 && ch < v.nch ? v.ch[ch].label : "ALL", ls_rfs_metric_name(m));
        tui_put_str(sf, body, body.x, row, line, TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
        tui_put_str(sf, body, body.x + w - 5, row, "[CH]", TUI_ATTR(TUI_BLACK, TUI_CYAN));
        row++;
        if (any) {
            int x = body.x;
            snprintf(line, sizeof(line), "%.0f ", lo);
            tui_put_str(sf, body, x, row, line, dim); x += (int)strlen(line);
            for (int s = 0; s < 5; s++) { tui_put_char(sf, body, x++, row, ls_rfmap_glyph(s), TUI_ATTR(ls_rfmap_hue(s), TUI_BLACK)); x++; }
            snprintf(line, sizeof(line), "%.0f %s", hi, m == LS_RFS_OCC ? "%" : m == LS_RFS_OVER ? "dB" : v.unit);
            tui_put_str(sf, body, x, row, line, dim);
        } else tui_put_str(sf, body, body.x, row, "no rows for this channel", dim);
        row++;
    }
    if (fox) {
        ls_fox_hunt_t h; ls_fox_get(&h);
        ls_fox_fix_t f;
        for (int x = 0; x < w; x++) tui_put_char(sf, body, body.x + x, row, ' ', ink);
        if (ls_fox_fix_now(&f)) LS_SNPRINTF(t_ls_rfmap_fox_d_brg_x, line, sizeof(line), "FOX %d brg  X +/-%.0f m", h.n, 2 * f.major_m);
        else LS_SNPRINTF(t_ls_rfmap_fox_d_bearing_s, line, sizeof(line), "FOX %d bearing%s, no fix yet", h.n, h.n == 1 ? "" : "s");
        tui_put_str(sf, body, body.x, row, line, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        row++;
    }
    r.w = w; r.h = row - body.y;
    return r;
}

/* ----------------------------------------------------------------- pick -- */

static int s_pick_n;
static void pick_done(int i)
{
    int ch; ls_rfs_metric_t m; ls_rfs_shown(&ch, &m);
    if (i == 0) ch = -1;
    else if (i <= s_pick_n) ch = i - 1;
    else if (i - s_pick_n - 1 < LS_RFS_METRICS) m = (ls_rfs_metric_t)(i - s_pick_n - 1);
    ls_rfs_show(ch, m);
}

void ls_rfmap_pick(void)
{
    EXT_RAM_BSS_ATTR static ls_rfs_view_t v;
    ls_rfs_view(&v);
    int ch; ls_rfs_metric_t m; ls_rfs_shown(&ch, &m);
    s_pick_n = v.nch;
    ls_picker_open("HEAT", pick_done);
    ls_picker_add(ch < 0 ? "> ALL CHANNELS" : "ALL CHANNELS", LS_TEXT(t_ls_rfmap_the_loudest_channel_in, "the loudest channel in each cell"));
    for (int i = 0; i < v.nch; i++) {
        char label[24], detail[48];
        snprintf(label, sizeof(label), "%s%s", i == ch ? "> " : "", v.ch[i].label);
        LS_SNPRINTF(t_ls_rfmap_4f_mhz_1f_khz, detail, sizeof(detail), "%.4f MHz, %.1f kHz wide", v.ch[i].hz / 1e6, v.ch[i].bw_hz / 1e3);
        ls_picker_add(label, detail);
    }
    static const char *const WHAT[LS_RFS_METRICS] = { "the strongest heard there", "the average heard there",
                                                      "how often it stood over its floor", "how far over the floor, on average" };
    for (int k = 0; k < LS_RFS_METRICS; k++) {
        char label[24];
        snprintf(label, sizeof(label), "%sSHOW %s", k == (int)m ? "> " : "", ls_rfs_metric_name(k));
        ls_picker_add(label, WHAT[k]);
    }
    ls_picker_select(ch < 0 ? 0 : ch + 1);
}
