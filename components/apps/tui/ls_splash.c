#include "ls_splash.h"
#include "ls_map_ink.h"
#include "ls_splash_world.h"
#include "ls_tui_ui.h"
#include "ls_wordmark.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const uint8_t SPLASH_LADDER[5] = {
    TUI_BLUE, TUI_BLUE | TUI_BRIGHT, TUI_CYAN,
    TUI_CYAN | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT,
};
#define SPLASH_FADE_PCT 55

/* A sextant dot is about 0.87 as wide as it is tall on this font, so the
   globe's vertical radius in dots is squeezed by it to come out round. */
#define DOT_ASPECT 0.87f
/* The view settles over North America: the boards are mostly there. */
#define HOME_LAT   28.0f
#define HOME_LON  -96.0f
#define SPIN_DEG  150.0f
/* Sunlight from the upper left, toward the viewer: the lit side of the globe
   is the bright one, and the limb fades round to the night side. */
#define SUN_X -0.55f
#define SUN_Y  0.50f
#define SUN_Z  0.67f

/* Dot priorities. The ink canvas draws 3 and under as braille, which is the
   fine line a vector globe wants; the later write wins a tie. */
enum { P_STAR = 1, P_GRID = 2, P_LAND = 3 };

static const char *s_caption;
void ls_splash_set_caption(const char *text) { s_caption = text; }
static const char *s_status;
void ls_splash_set_status(const char *text) { s_status = text; }

typedef struct {
    float cx, cy, r;            /* centre and radius, dots */
    float slo, clo, sla, cla;   /* view centre */
    int wx0, wy0, wx1, wy1;     /* title window, dots, inclusive */
    int sy0, sy1;               /* the progress line's cell row, dots */
} view_t;

static float ease_out(float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    const float u = 1 - t;
    return 1 - u * u * u;
}

/* Unit vector to the view: x right, y up, z toward the viewer. */
static void turn(const view_t *v, float x, float y, float z, float out[3])
{
    const float x1 = x * v->clo - z * v->slo;
    const float z1 = x * v->slo + z * v->clo;
    out[0] = x1;
    out[1] = y * v->cla - z1 * v->sla;
    out[2] = y * v->sla + z1 * v->cla;
}

static float lit(const float n[3]) { return n[0] * SUN_X + n[1] * SUN_Y + n[2] * SUN_Z; }

static bool in_window(const view_t *v, int x, int y)
{
    return x >= v->wx0 && x <= v->wx1 && y >= v->wy0 && y <= v->wy1;
}

/* Behind the title the globe goes on, in grey, so the letters stand clear
   of it without a hole in the planet. */
static void dot(const view_t *v, int x, int y, uint8_t fg, uint8_t prio)
{
    if (in_window(v, x, y)) {
        if (y >= v->sy0 && y <= v->sy1) return;
        fg = LS_FAINT_FG; prio = P_STAR;
    }
    ls_ink_dot(x, y, TUI_ATTR(fg, TUI_BLACK), prio);
}

static void line(const view_t *v, int x0, int y0, int x1, int y1, uint8_t fg, uint8_t prio)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 2000; guard++) {
        dot(v, x0, y0, fg, prio);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static bool project(const view_t *v, const float n[3], int *x, int *y)
{
    if (n[2] <= 0) return false;
    *x = (int)lrintf(v->cx + v->r * n[0]);
    *y = (int)lrintf(v->cy - v->r * DOT_ASPECT * n[1]);
    return true;
}

static void stars(const view_t *v, int w, int h, int frame)
{
    const int count = w * h / 170;
    uint32_t seed = 0x9E3779B9u;
    for (int i = 0; i < count; i++) {
        seed = seed * 1664525u + 1013904223u;
        const int x = (int)(seed >> 8) % w;
        seed = seed * 1664525u + 1013904223u;
        const int y = (int)(seed >> 8) % h;
        const float dx = (x - v->cx) / v->r, dy = (y - v->cy) / (v->r * DOT_ASPECT);
        if (dx * dx + dy * dy < 1.25f || in_window(v, x, y)) continue;
        /* One star in nine flares for a few frames now and then. */
        const bool flare = i % 9 == 0 && ((frame / 5 + i) % 7) == 0;
        ls_ink_dot(x, y, TUI_ATTR(flare ? TUI_WHITE | TUI_BRIGHT : LS_FAINT_FG, TUI_BLACK), P_STAR);
    }
}

static void graticule(const view_t *v)
{
    float n[3];
    int x, y;
    for (int lo = -180; lo < 180; lo += 30)
        for (int la = -84; la <= 84; la += 3) {
            const float fl = la * (float)M_PI / 180, fo = lo * (float)M_PI / 180;
            turn(v, cosf(fl) * sinf(fo), sinf(fl), cosf(fl) * cosf(fo), n);
            if (project(v, n, &x, &y))
                dot(v, x, y, lit(n) > 0.25f ? TUI_BLUE | TUI_BRIGHT : TUI_BLUE, P_GRID);
        }
    for (int la = -60; la <= 60; la += 30)
        for (int lo = -180; lo < 180; lo += 3) {
            const float fl = la * (float)M_PI / 180, fo = lo * (float)M_PI / 180;
            turn(v, cosf(fl) * sinf(fo), sinf(fl), cosf(fl) * cosf(fo), n);
            if (project(v, n, &x, &y))
                dot(v, x, y, la == 0 ? TUI_MAGENTA : lit(n) > 0.25f ? TUI_BLUE | TUI_BRIGHT : TUI_BLUE,
                    P_GRID);
        }
}

static void limb(const view_t *v)
{
    /* The edge of the disc, bright where the sun reaches it, and a thin
       broken halo outside it on the lit side. */
    const int steps = (int)(2 * M_PI * v->r) + 12;
    for (int i = 0; i < steps; i++) {
        const float a = 2 * (float)M_PI * i / steps, ca = cosf(a), sa = sinf(a);
        const float l = ca * SUN_X + sa * SUN_Y;
        dot(v, (int)lrintf(v->cx + v->r * ca), (int)lrintf(v->cy - v->r * DOT_ASPECT * sa),
            l > 0.2f ? TUI_CYAN | TUI_BRIGHT : l > -0.4f ? TUI_CYAN : TUI_BLUE, P_GRID);
        if (l > 0 && i % 3 == 0) {
            const float h = v->r + 3;
            dot(v, (int)lrintf(v->cx + h * ca), (int)lrintf(v->cy - h * DOT_ASPECT * sa),
                l > 0.6f ? TUI_CYAN : TUI_BLUE, P_GRID);
        }
    }
}

static void land(const view_t *v)
{
    for (unsigned r = 0; r < ls_splash_world_rings; r++) {
        bool have = false;
        int px = 0, py = 0;
        for (unsigned i = ls_splash_world_start[r]; i < ls_splash_world_start[r + 1]; i++) {
            const int16_t *p = ls_splash_world_xyz[i];
            float n[3];
            int x, y;
            turn(v, p[0] / 32767.0f, p[1] / 32767.0f, p[2] / 32767.0f, n);
            if (!project(v, n, &x, &y)) { have = false; continue; }
            const float l = lit(n);
            const uint8_t fg = l > 0.45f ? TUI_GREEN | TUI_BRIGHT : l > 0.0f ? TUI_GREEN : TUI_CYAN;
            if (have) line(v, px, py, x, y, fg, P_LAND);
            else dot(v, x, y, fg, P_LAND);
            px = x; py = y; have = true;
        }
    }
}

/* Amber corner marks on the title window. */
static void corners(const view_t *v)
{
    const uint8_t at = TUI_ATTR(TUI_YELLOW, TUI_BLACK);
    const int xs[2] = { v->wx0, v->wx1 }, ys[2] = { v->wy0, v->wy1 };
    for (int k = 0; k < 4; k++) {
        const int x = xs[k & 1], y = ys[k >> 1];
        const int sx = k & 1 ? -1 : 1, sy = k >> 1 ? -1 : 1;
        for (int i = 0; i < 6; i++) ls_ink_dot(x + sx * i, y, at, P_LAND);
        for (int i = 1; i < 4; i++) ls_ink_dot(x, y + sy * i, at, P_LAND);
    }
}

void ls_splash_draw(tui_surface *sf, int cols, int rows, int frame,
                       int frames)
{
    tui_rect all = tui_surface_rect(sf);
    tui_frame_begin(sf);

    if (frames < 1) frames = 1;
    const int pct = frame * 100 / frames;
    const float t = (float)frame / frames;

    /* Split before anything is positioned: the assembly's height depends on
       whether the mark needed two lines, and its top on the height. */
    char line_a[32], line_b[32];
    const bool compact = cols - 2 < ls_wordmark_width("TERMINAL");
    int (*mark_width)(const char *) = compact ? ls_wordmark_compact_width : ls_wordmark_width;
    void (*mark_row)(tui_surface *, tui_rect, int, int, int, const char *, uint8_t) =
        compact ? ls_wordmark_compact_row : ls_wordmark_row;
    const bool two = (compact ? ls_wordmark_compact_split : ls_wordmark_split)("TERMINAL BAY", cols - 2,
                                       line_a, sizeof(line_a),
                                       line_b, sizeof(line_b));
    const int mark_rows = two ? (LS_WORDMARK_ROWS * 2 + 1) : LS_WORDMARK_ROWS;
    int top = rows / 2 - (mark_rows + 7) / 2;
    int mw = mark_width(line_a);
    if (two && mark_width(line_b) > mw) mw = mark_width(line_b);
    /* A wide grid puts the globe right of centre at full height and the
       title left of it, over its limb; a narrow one stacks both centred. */
    const int w = cols * 2, h = rows * 3;
    float full = (h / 2.0f - 1) / DOT_ASPECT;
    const bool wide = w > full * 4.2f && cols * 36 / 100 > mw / 2 + 2;
    const int tc = wide ? cols * 36 / 100 : cols / 2;
    const float gx = wide ? w * 0.71f : w / 2.0f;
    if (full > w / 2.0f - 2) full = w / 2.0f - 2;
    if (wide && full > w - gx - 2) full = w - gx - 2;
    /* A tall grid has room for the whole globe above the title. */
    float gy = h / 2.0f;
    if (!wide) {
        const float side = w / 2.0f - 2;
        const int globe_rows = (int)(2 * side * DOT_ASPECT / 3) + 2;
        if (rows >= globe_rows + mark_rows + 9) {
            full = side;
            gy = globe_rows * 3 / 2.0f;
            top = globe_rows + (rows - globe_rows - (mark_rows + 7)) / 2;
        }
    }
    const int rw = mw, rx = tc - rw / 2;
    const int win_w = mw + 6;
    const int win_x = tc - win_w / 2;

    /* The globe: it closes in, tilts down and turns to home, slowing as it
       arrives, so the last frames are nearly still for the hand-off. */
    const float zoom = ease_out(t / 0.6f), spin = ease_out(t);
    const float lon = (HOME_LON - SPIN_DEG * (1 - spin)) * (float)M_PI / 180;
    const float lat = (HOME_LAT + 14 * (1 - zoom)) * (float)M_PI / 180;
    view_t v = {
        .cx = gx, .cy = gy, .r = full * (0.62f + 0.38f * zoom),
        .slo = sinf(lon), .clo = cosf(lon), .sla = sinf(lat), .cla = cosf(lat),
        .wx0 = win_x * 2, .wy0 = (top - 1) * 3 + 1,
        .wx1 = (win_x + win_w) * 2 - 1, .wy1 = (top + mark_rows + 8) * 3 + 1,
        .sy0 = (top + mark_rows + 5) * 3, .sy1 = (top + mark_rows + 5) * 3 + 2,
    };

    if (ls_ink_begin(all)) {
        stars(&v, w, h, frame);
        graticule(&v);
        limb(&v);
        land(&v);
        corners(&v);

        /* Progress as a hairline through the window: a dotted track, the
           part already done graded blue to white, and a bright head. */
        const int py = (top + mark_rows + 5) * 3 + 1, px0 = rx * 2, pw = rw * 2;
        const int done = pw * frame / frames;
        for (int i = 0; i < pw; i++) {
            uint8_t fg;
            if (i < done) fg = i >= done - 2 ? TUI_WHITE | TUI_BRIGHT
                             : i * 3 < pw ? TUI_BLUE | TUI_BRIGHT
                             : i * 3 < pw * 2 ? TUI_CYAN : TUI_CYAN | TUI_BRIGHT;
            else if (i % 2) continue;
            else fg = LS_FAINT_FG;
            ls_ink_dot(px0 + i, py, TUI_ATTR(fg, TUI_BLACK), P_LAND);
        }
        ls_ink_flush(sf, tui_rect_make(0, -1, 0, 0));
    }

    for (int r = 0; r < LS_WORDMARK_ROWS; r++) {
        /* Row 0 is the brightest, so its target is the top of the ladder. */
        const int target = LS_WORDMARK_ROWS - 1 - r;
        int lit_row = (pct >= SPLASH_FADE_PCT)
                ? LS_WORDMARK_ROWS - 1
                : pct * LS_WORDMARK_ROWS / SPLASH_FADE_PCT;
        if (lit_row > target) lit_row = target;
        const uint8_t at = TUI_ATTR(SPLASH_LADDER[lit_row], TUI_BLACK);

        mark_row(sf, all, tc - mark_width(line_a) / 2,
                        top, r, line_a, at);
        if (two)
            mark_row(sf, all,
                            tc - mark_width(line_b) / 2,
                            top + LS_WORDMARK_ROWS + 1, r, line_b, at);
    }

    const char *sub = "L A K E S H A R K";
    tui_put_str(sf, all, tc - (int)strlen(sub) / 2, top + mark_rows + 1,
                sub, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    const char *tag = "SDR  FIELD  TERMINAL";
    if (pct >= 30)
        tui_put_str(sf, all, tc - (int)strlen(tag) / 2, top + mark_rows + 3,
                    tag, TUI_ATTR(TUI_YELLOW, TUI_BLACK));

    static const char *const STAGE[] = { "PANEL", "RADIO", "USB HOST",
                                         "KEYBOARD", "READY" };
    int stage = frame * 5 / frames;
    if (stage > 4) stage = 4;
    const char *label = s_status && *s_status ? s_status : STAGE[stage];
    const bool ready = s_status && *s_status ? frame >= frames : stage == 4;
    tui_put_str(sf, all, tc - (int)strlen(label) / 2,
                top + mark_rows + 7, label,
                TUI_ATTR(ready ? (TUI_GREEN | TUI_BRIGHT) : TUI_WHITE, TUI_BLACK));

    /* Where the globe is looking, as it turns, and what is booting. */
    if (rows >= 12) {
        char at[40];
        const float lo = lon * 180 / (float)M_PI, la = lat * 180 / (float)M_PI;
        snprintf(at, sizeof(at), "%5.1f%c %6.1f%c", fabsf(la), la < 0 ? 'S' : 'N',
                 fabsf(lo), lo < 0 ? 'W' : 'E');
        tui_put_str(sf, all, 1, rows - 1, at, TUI_ATTR(TUI_CYAN, TUI_BLACK));
        if (s_caption && *s_caption) {
            const int n = (int)strlen(s_caption);
            if (n + (int)strlen(at) + 4 < cols)
                tui_put_str(sf, all, cols - 1 - n, rows - 1, s_caption, LS_ATTR_FAINT);
        }
    }
}
