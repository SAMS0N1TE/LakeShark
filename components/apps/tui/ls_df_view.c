#include "ls_df_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "ls_tui.h"
#include "ls_tui_ui.h"

#define RAD 0.017453292519943295f
#define MAX_DOTS (160 * 120)

EXT_RAM_BSS_ATTR static ls_dot_t s_dots[MAX_DOTS];
struct radar_blip { float x, y, r2; uint8_t ink; };
EXT_RAM_BSS_ATTR static struct radar_blip s_radar_blips[LS_COMPASS_BLIPS];

static float wrap180(float a) { return fmodf(a + 540.0f, 360.0f) - 180.0f; }

static float bayer(int x, int y)
{
    static const uint8_t M[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    return (M[y & 3][x & 3] + 0.5f) / 16.0f;
}

/* ----------------------------------------------------------------- radar -- */

typedef struct {
    float cx, cy, R;               /* pixels */
    float line;                    /* one dot, in radii */
    float heading;
    const struct radar_blip *b;    /* s_radar_blips: kept off the TUI task's stack */
    int blips;
    const ls_df_radar_t *r;
} radar_view_t;

static float cone_length(float over_db)
{
    return 0.1f + 0.9f * fmaxf(0.0f, fminf(1.0f, over_db / LS_DF_VIEW_SCALE_DB));
}

static void radar_dot(const radar_view_t *v, float X, float Y, int dx, int dy, ls_dot_t *d)
{
    const ls_df_radar_t *rd = v->r;
    d->set = false;
    const float r = sqrtf(X * X + Y * Y);
    const float w = v->line;
    if (r > 1.0f + w) return;
    const float phi = atan2f(X, Y) / RAD;
    const float az = isfinite(v->heading) ? phi + v->heading : phi;
#define SET(c) do { d->set = true; d->colour = (c); return; } while (0)
    if (rd->facing && r > 0.04f && Y > 0 && fabsf(X) < w * 0.6f) SET(TUI_GREEN | TUI_BRIGHT);
    for (int b = 0; b < v->blips; b++) {
        const float ex = X - v->b[b].x, ey = Y - v->b[b].y;
        if (ex * ex + ey * ey < v->b[b].r2) SET(v->b[b].ink);
    }
    if (isfinite(rd->pick) && r > 0.05f && fabsf(wrap180(az - rd->pick)) * RAD * r < w * 0.6f && bayer(dx, dy) < 0.6f)
        SET(TUI_MAGENTA | TUI_BRIGHT);
    for (int i = 0; i < rd->cones; i++) {
        const ls_df_cone_t *c = &rd->cone[i];
        if (!c->valid || !c->shown) continue;
        if (r > 0.05f && r < cone_length(c->over_db) && fabsf(wrap180(az - c->bearing)) * RAD * r < w * 0.6f)
            SET(TUI_RED | TUI_BRIGHT);
    }
    for (int i = 0; i < rd->cones; i++) {
        const ls_df_cone_t *c = &rd->cone[i];
        if (!c->valid) continue;
        const float s = fmaxf(4.0f, c->spread), off = fabsf(wrap180(az - c->bearing)), L = cone_length(c->over_db);
        if (off > s + 3.0f || r > L + w) continue;
        if (fabsf(off - s) * RAD * r < w * 0.7f || (off <= s && fabsf(r - L) < w * 0.7f)) SET(c->colour | TUI_BRIGHT);
        if (off <= s && r <= L && bayer(dx, dy) < (c->shown ? 0.45f : 0.3f)) SET(c->colour);
    }
    if (rd->lobe) {
        const int bin = ((int)lroundf((az + 720.0f) / 5.0f)) % LS_DF_BINS;
        const float lv = fmaxf(0.0f, fminf(1.0f, rd->lobe[bin]));
        if (lv > 0) {
            const float rl = 0.08f + 0.9f * lv;
            if (fabsf(r - rl) < w * 0.7f) SET(TUI_BLUE | TUI_BRIGHT);
            if (r < rl && bayer(dx, dy) < 0.3f) SET(TUI_BLUE);
        }
    }
    if (rd->recent && r > 0.04f) {
        const float rv = rd->recent[((int)lroundf((az + 720.0f) / 5.0f)) % LS_DF_BINS];
        if (rv > 0 && bayer(dx, dy) < 0.5f * rv) SET(TUI_GREEN);
    }
    if (fabsf(r - 1.0f) < w * 0.7f) SET(TUI_GREEN);
    for (int k = 1; k <= 2; k++)
        if (fabsf(r - k / 3.0f) < w * 0.5f && bayer(dx, dy) < 0.5f) SET(TUI_BLACK | TUI_BRIGHT);
    if ((fabsf(X) < w * 0.4f || fabsf(Y) < w * 0.4f) && bayer(dx, dy) < 0.25f) SET(TUI_BLACK | TUI_BRIGHT);
#undef SET
}

static void put_at(tui_surface *sf, tui_rect area, const radar_view_t *v, float phi, float r,
                   const char *text, uint8_t attr, int cw, int ch)
{
    const float px = v->cx + sinf(phi * RAD) * r * v->R, py = v->cy - cosf(phi * RAD) * r * v->R;
    tui_put_str(sf, area, area.x + (int)floorf(px / cw) - (int)strlen(text) / 2, area.y + (int)floorf(py / ch), text, attr);
}

void ls_df_radar_draw(tui_surface *sf, tui_rect area, const ls_df_radar_t *rd)
{
    if (!sf || !rd || area.w < 12 || area.h < 8) return;
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    if (cw < 1) cw = 10;
    if (ch < 1) ch = 17;
    tui_put_str(sf, area, area.x + 1, area.y, "RADAR  top up", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    EXT_RAM_BSS_ATTR static char scale[40];
    snprintf(scale, sizeof(scale), "rings %.0f/%.0f/%.0f dB over noise", LS_DF_VIEW_SCALE_DB / 3,
             LS_DF_VIEW_SCALE_DB * 2 / 3, LS_DF_VIEW_SCALE_DB);
    if ((int)strlen(scale) + 16 <= area.w) tui_put_str(sf, area, area.x + area.w - 1 - (int)strlen(scale), area.y, scale, LS_ATTR_DIM);
    tui_rect pic = tui_rect_make(area.x, area.y + 1, area.w, area.h - 1);
    const int dots_w = pic.w * 2, dots_h = pic.h * 3;
    if (dots_w * dots_h > MAX_DOTS) return;
    const float sub_w = cw / 2.0f, sub_h = ch / 3.0f;
    const float wpx = pic.w * (float)cw, hpx = pic.h * (float)ch;
    radar_view_t v = { .r = rd, .heading = rd->heading, .b = s_radar_blips };
    /* Room round the rim for the compass letters. */
    v.R = fminf(wpx / 2 - 1.6f * cw, hpx / 2 - 1.2f * ch);
    if (v.R < 20) return;
    v.cx = wpx / 2; v.cy = hpx / 2;
    v.line = 1.2f * fmaxf(sub_w, sub_h) / v.R;
    const int nb = rd->blips ? (rd->blip_count < LS_COMPASS_BLIPS ? rd->blip_count : LS_COMPASS_BLIPS) : 0;
    for (int b = 0; b < nb; b++) {
        const ls_compass_blip_t *h = &rd->blips[b];
        if (!isfinite(h->bearing)) continue;
        const float rel = (h->bearing - (isfinite(rd->heading) ? rd->heading : 0)) * RAD;
        const float rb = 0.12f + 0.85f * fmaxf(0.0f, fminf(1.0f, h->strength));
        const float size = fmaxf(0.05f, v.line * 1.8f) * (1.0f - 0.45f * fmaxf(0.0f, fminf(1.0f, h->age)));
        s_radar_blips[v.blips] = (struct radar_blip){ rb * sinf(rel), rb * cosf(rel), size * size,
                                                      h->age < 0.4f ? (TUI_GREEN | TUI_BRIGHT) : TUI_GREEN };
        v.blips++;
    }
    for (int j = 0; j < dots_h; j++)
        for (int i = 0; i < dots_w; i++) {
            const float px = (i + 0.5f) * sub_w, py = (j + 0.5f) * sub_h;
            radar_dot(&v, (px - v.cx) / v.R, (v.cy - py) / v.R, i, j, &s_dots[j * dots_w + i]);
        }
    ls_dots_blit(sf, pic, s_dots, dots_w);
    /* Compass letters on the rim, where north and the rest now lie. */
    const float hd = isfinite(rd->heading) ? rd->heading : 0.0f;
    static const char *const CARD[] = { "N", "E", "S", "W" };
    for (int i = 0; i < 4; i++)
        put_at(sf, pic, &v, i * 90.0f - hd, 1.1f, CARD[i],
               TUI_ATTR((i ? TUI_WHITE : TUI_RED) | TUI_BRIGHT, TUI_BLACK), cw, ch);
    for (int i = 0; i < rd->cones; i++) {
        const ls_df_cone_t *c = &rd->cone[i];
        if (!c->valid || !c->tag) continue;
        const char t[2] = { c->tag, 0 };
        put_at(sf, pic, &v, c->bearing - hd, fminf(0.95f, cone_length(c->over_db) + 0.06f), t,
               TUI_ATTR(TUI_BLACK, c->colour | TUI_BRIGHT), cw, ch);
    }
    if (!isfinite(rd->heading)) put_at(sf, pic, &v, 180.0f, 0.5f, "NO HEADING", LS_ATTR_DIM, cw, ch);
}

/* ------------------------------------------------------------------ heat -- */

uint8_t ls_df_heat_cell(float over_db, bool heard)
{
    if (!heard || !isfinite(over_db)) return 0;
    const float f = fmaxf(0.0f, fminf(1.0f, over_db / LS_DF_VIEW_SCALE_DB));
    return (uint8_t)(1 + lroundf(f * 254.0f));
}

static void heat_glyph(uint8_t v, char *c, uint8_t *attr)
{
    static const char RAMP[] = ".:-=+*#%@";
    static const uint8_t INK[9] = { TUI_BLUE, TUI_BLUE, TUI_BLUE | TUI_BRIGHT, TUI_BLUE | TUI_BRIGHT,
                                    TUI_CYAN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT,
                                    TUI_RED | TUI_BRIGHT, TUI_RED | TUI_BRIGHT };
    if (!v) { *c = ' '; *attr = LS_ATTR_DIM; return; }
    int i = (v - 1) * 9 / 255;
    if (i > 8) i = 8;
    *c = RAMP[i]; *attr = TUI_ATTR(INK[i], TUI_BLACK);
}

int ls_df_heat_draw(tui_surface *sf, tui_rect area, const ls_df_heat_t *h)
{
    if (!sf || !h || area.w < 20 || area.h < 4) return 0;
    const int lw = area.w >= 44 ? 12 : 7;
    const int gx = area.x + lw, gw = area.w - lw - 1;
    int y = area.y;
    tui_put_str(sf, area, area.x + 1, y++, h->title ? h->title : "HEAT", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    /* The bearing axis: true degrees, north at the left. */
    for (int c = 0; c < gw; c++) {
        const float a0 = c * 360.0f / gw, a1 = (c + 1) * 360.0f / gw;
        char g = '-';
        uint8_t at = LS_ATTR_DIM;
        static const char L[] = "NESW";
        for (int q = 0; q < 4; q++)
            if (q * 90.0f >= a0 && q * 90.0f < a1) { g = L[q]; at = TUI_ATTR((q ? TUI_WHITE : TUI_RED) | TUI_BRIGHT, TUI_BLACK); }
        tui_put_char(sf, area, gx + c, y, g, at);
    }
    y++;
    const int room = area.y + area.h - y - 2;
    const int rows = h->rows < room ? h->rows : room;
    for (int r = 0; r < rows; r++, y++) {
        EXT_RAM_BSS_ATTR static char label[16];
        snprintf(label, sizeof(label), "%-*.*s", lw - 1, lw - 1, h->labels && h->labels[r] ? h->labels[r] : "");
        tui_put_str(sf, area, area.x, y, label,
                    r == h->selected ? TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT) : LS_ATTR_DIM);
        const uint8_t *row = h->cells + r * LS_DF_BINS;
        for (int c = 0; c < gw; c++) {
            /* The strongest bin the column spans. */
            const int b0 = c * LS_DF_BINS / gw, b1 = (c + 1) * LS_DF_BINS / gw;
            uint8_t v = row[b0 % LS_DF_BINS];
            for (int b = b0 + 1; b < b1; b++) if (row[b] > v) v = row[b];
            char g; uint8_t at; heat_glyph(v, &g, &at);
            tui_put_char(sf, area, gx + c, y, g, at);
        }
    }
    /* Where the board faces, and a picked hit. */
    if (isfinite(h->heading)) {
        const int c = (int)(fmodf(h->heading + 360.0f, 360.0f) * gw / 360.0f);
        tui_put_char(sf, area, gx + c, y, '^', TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }
    if (isfinite(h->pick)) {
        const int c = (int)(fmodf(h->pick + 360.0f, 360.0f) * gw / 360.0f);
        tui_put_char(sf, area, gx + c, y, '!', TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
    }
    tui_put_str(sf, area, area.x, y, "top ^", LS_ATTR_DIM);
    y++;
    EXT_RAM_BSS_ATTR static char legend[64];
    snprintf(legend, sizeof(legend), " .:-=+*#%%@  0 to %.0f dB over noise, blank unheard", LS_DF_VIEW_SCALE_DB);
    ls_safe_line(sf, area, y++, legend, LS_ATTR_DIM);
    return y - area.y;
}
