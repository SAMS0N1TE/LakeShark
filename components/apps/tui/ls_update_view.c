/* UPDATE, in SYSTEM; Settings > DEVICE > Update opens it too.

   The whole page moves with the update. Behind everything a field that says
   what is happening: rings going out while the board joins WiFi, packets
   circling the server while it reads the manifest, hex raining down while
   the image streams in, scan bands while it is checked, confetti when the
   new build is up, red static when something failed. In front of it the
   server, the pipe and the board (stacked in portrait, side by side in
   landscape), the percentage in figures as tall as a thumb, the steps, the
   bar, and what is being installed. When there is something to do, a big
   button breathing under the word says what: INSTALL, RETRY, CHECK, or
   RESTART once the new build is written and counting down to its restart.

   Every effect is worked out from the clock and the cell, with no state
   kept between frames, so nothing drifts and nothing needs resetting. The
   one exception is the download: its rain and pipe scroll by counting
   drawn frames (s_frame), not by the clock, because the frames arrive at an
   even rate near any wall-time step and a clock step would give them none
   or two cells at a time.
   Everything about the update itself comes from the ota.* values
   main/ls_ota.c publishes, and ENTER steps it with ota.step. */

#include "ls_update_view.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "ls_action.h"
#include "ls_glyph.h"
#include "ls_motion.h"
#include "ls_tui_ui.h"
#include "ls_value.h"
#include "ls_ota_core.h"

#define A(fg, bg) TUI_ATTR(fg, bg)
#define BR(c)     ((uint8_t)((c) | TUI_BRIGHT))

static const char HEX[] = "0123456789abcdef";

static long    s_got_prev = -1;
static int64_t s_got_us;
static float   s_rate;                  /* bytes a second, smoothed */
static uint32_t s_frame;                /* frames drawn while downloading: the
                                           rain and the pipe step on this */

/* Where a tap does the same as the button, as last drawn: the big button,
   and READY itself. */
static tui_rect s_cta, s_hero;
static bool     s_cta_on, s_hero_on;

/* --------------------------------------------------------------- values -- */

static bool get_int(const char *path, long *v)
{
    ls_val_t x;
    if (!ls_value_read(path, &x, NULL) || x.kind != LS_VAL_INT) return false;
    *v = x.i;
    return true;
}

static void get_text(const char *path, char *out, size_t cap)
{
    ls_val_t x;
    out[0] = 0;
    if (ls_value_read(path, &x, NULL) && x.kind == LS_VAL_TEXT && x.s)
        snprintf(out, cap, "%s", x.s);
}

/* "2.8.4-rc1-g47096733100f" -> "2.8.4-rc1" */
static void release_of(const char *build, char *out, size_t cap)
{
    snprintf(out, cap, "%s", build);
    char *g = strstr(out, "-g");
    if (g) *g = 0;
}

/* Seconds the written build still waits to be restarted: 0 when it is not
   waiting, or the restart has begun. */
static long restart_left(void)
{
    long v = 0;
    return get_int("ota.restart", &v) && v > 0 ? v : 0;
}

/* Written, and waiting for RESTART or the countdown. */
static bool ready_to_restart(long st)
{
    return st == LS_OTA_ST_RESTARTING && restart_left() > 0;
}

static bool working(long st)
{
    return st == LS_OTA_ST_WIFI || st == LS_OTA_ST_CHECKING || st == LS_OTA_ST_DOWNLOADING ||
           st == LS_OTA_ST_VERIFYING || (st == LS_OTA_ST_RESTARTING && !ready_to_restart(st));
}

static uint8_t hue_of(long st)
{
    switch (st) {
    case LS_OTA_ST_CURRENT:   return TUI_GREEN;
    case LS_OTA_ST_UPDATED:   return BR(TUI_GREEN);
    case LS_OTA_ST_AVAILABLE: return BR(TUI_MAGENTA);
    case LS_OTA_ST_FAILED:    return BR(TUI_RED);
    case LS_OTA_ST_IDLE:      return TUI_CYAN;
    case LS_OTA_ST_VERIFYING: return BR(TUI_CYAN);
    case LS_OTA_ST_RESTARTING: return ready_to_restart(st) ? BR(TUI_GREEN) : BR(TUI_YELLOW);
    default:                  return BR(TUI_YELLOW);
    }
}

/* The same number every frame for the same place and step, so things move
   rather than flicker. */
static uint32_t noise(int i, int step)
{
    uint32_t h = (uint32_t)i * 2654435761u ^ (uint32_t)step * 40503u ^ 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h;
}

static void put(tui_surface *sf, tui_rect clip, int x, int y, char ch, uint8_t at)
{
    if (x >= clip.x && y >= clip.y && x < clip.x + clip.w && y < clip.y + clip.h)
        tui_put_char(sf, clip, x, y, ch, at);
}

/* tui_fill, kept inside `clip` the way put() is. */
static void fill(tui_surface *sf, tui_rect clip, tui_rect r, char ch, uint8_t at)
{
    const int x0 = r.x > clip.x ? r.x : clip.x, y0 = r.y > clip.y ? r.y : clip.y;
    const int x1 = r.x + r.w < clip.x + clip.w ? r.x + r.w : clip.x + clip.w;
    const int y1 = r.y + r.h < clip.y + clip.h ? r.y + r.h : clip.y + clip.h;
    if (x1 > x0 && y1 > y0) tui_fill(sf, tui_rect_make(x0, y0, x1 - x0, y1 - y0), ch, at);
}

static void centre(tui_surface *sf, tui_rect clip, int y, const char *s, uint8_t at)
{
    char l[128];
    snprintf(l, sizeof(l), "%.*s", clip.w > 0 ? clip.w : 0, s);
    tui_put_str(sf, clip, clip.x + (clip.w - (int)strlen(l)) / 2, y, l, at);
}

/* ----------------------------------------------------------- background -- */

/* Stars drifting left: the page at rest. */
static void bg_stars(tui_surface *sf, tui_rect r, int64_t t)
{
    const int n = r.w * r.h / 28;
    for (int i = 0; i < n; i++) {
        const int speed = 220 + (int)(noise(i, 2) % 500);
        int64_t drift = ((int64_t)(noise(i, 1) % (uint32_t)r.w) - t / speed) % r.w;
        if (drift < 0) drift += r.w;
        const int x = (int)drift;
        const int y = (int)(noise(i, 3) % (uint32_t)r.h);
        const bool twinkle = (t / 260 + i) % 11 == 0;
        put(sf, r, r.x + x, r.y + y, twinkle ? '+' : '.', twinkle ? A(BR(TUI_WHITE), TUI_BLACK)
                                                                   : A(TUI_BLUE, TUI_BLACK));
    }
}

/* Rings going out from (cx, cy): the board calling for its network. */
static void bg_rings(tui_surface *sf, tui_rect r, int cx, int cy, int64_t t)
{
    const float spread = (float)(t % 1600) / 1600.0f;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            const float dx = (float)(x - cx), dy = (float)(y - cy) * 2.0f;
            const float d = sqrtf(dx * dx + dy * dy);
            const float ring = fmodf(d - spread * 9.0f + 90.0f, 9.0f);
            if (ring < 0.8f && d < 60.0f) {
                const bool near = d < 20.0f;
                put(sf, r, x, y, near ? 'o' : '.', near ? A(BR(TUI_YELLOW), TUI_BLACK)
                                                        : A(TUI_YELLOW, TUI_BLACK));
            }
        }
}

/* Packets going round (cx, cy): the manifest being asked for. */
static void bg_orbit(tui_surface *sf, tui_rect r, int cx, int cy, int64_t t)
{
    for (int k = 0; k < 8; k++) {
        const float a = (float)t / 520.0f + (float)k * 0.785398f;
        const float rx = 13.0f + 3.0f * (float)(k % 2), ry = 4.0f + (float)(k % 2);
        for (int tail = 0; tail < 3; tail++) {
            const float b = a - (float)tail * 0.12f;
            const int x = cx + (int)lroundf(cosf(b) * rx), y = cy + (int)lroundf(sinf(b) * ry);
            put(sf, r, x, y, tail ? '.' : 'o', tail ? A(TUI_YELLOW, TUI_BLACK)
                                                    : A(BR(TUI_YELLOW), TUI_BLACK));
        }
    }
}

/* Sparkle around (cx, cy): something new is waiting. */
static void bg_sparkle(tui_surface *sf, tui_rect r, int cx, int cy, int64_t t)
{
    const int slice = (int)(t / 180);
    for (int i = 0; i < 46; i++) {
        const uint32_t h = noise(i, slice / 3);
        const int x = cx - 22 + (int)(h % 45), y = cy - 6 + (int)((h >> 8) % 13);
        const int life = (slice + i) % 6;
        if (life > 3) continue;
        static const char STAR[] = ".+*+";
        put(sf, r, x, y, STAR[life], life == 2 ? A(BR(TUI_WHITE), TUI_BLACK)
                                               : A(BR(TUI_MAGENTA), TUI_BLACK));
    }
}

/* Hex raining down the page while the image streams in. Each column falls
   one row every `every` frames (1 to 3, fixed for the column), so it keeps a
   steady beat whatever the frame time or the transfer rate; the glyphs
   change every second frame. */
static void bg_rain(tui_surface *sf, tui_rect r, uint32_t frame)
{
    for (int c = 0; c < r.w; c += 2) {
        const int len = 4 + (int)(noise(c, 4) % 9);
        const int period = r.h + len + (int)(noise(c, 7) % 12);
        const uint32_t every = 1 + noise(c, 5) % 3;
        const int head = (int)(((frame + noise(c, 9) % every) / every + noise(c, 6)) % (uint32_t)period);
        for (int k = 0; k < len; k++) {
            const int y = head - k;
            if (y < 0 || y >= r.h) continue;
            const char ch = HEX[noise(c * 131 + y, (int)(frame / 2) + k) % 16];
            const uint8_t at = k == 0 ? A(BR(TUI_WHITE), TUI_BLACK)
                             : k < 3  ? A(BR(TUI_GREEN), TUI_BLACK)
                                      : A(TUI_GREEN, TUI_BLACK);
            put(sf, r, r.x + c, r.y + y, ch, at);
        }
    }
}

/* Bands sweeping down: every byte being looked at. */
static void bg_bands(tui_surface *sf, tui_rect r, int64_t t)
{
    const int at = (int)(t / 45);
    for (int y = 0; y < r.h; y++) {
        const int band = (y * 3 + at) % 24;
        if (band > 2) continue;
        for (int x = 0; x < r.w; x++)
            put(sf, r, r.x + x, r.y + y, band == 1 ? LS_TUI_SHADE_50 : LS_TUI_SHADE_25,
                band == 1 ? A(BR(TUI_CYAN), TUI_BLACK) : A(TUI_CYAN, TUI_BLACK));
    }
}

/* The page coming apart and back: the board going down for the restart. */
static void bg_dissolve(tui_surface *sf, tui_rect r, int64_t t)
{
    const int level = (int)((t % 1400) * 256 / 1400);
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++)
            if ((int)(noise(y * 977 + x, 0) & 255) < level)
                put(sf, r, r.x + x, r.y + y, (noise(x, y) & 1) ? LS_TUI_SHADE_25 : LS_TUI_SHADE_50,
                    A(TUI_BLUE, TUI_BLACK));
}

/* Bursts of colour: the new build is up. */
static void bg_confetti(tui_surface *sf, tui_rect r, int64_t t)
{
    static const uint8_t PAL[] = { BR(TUI_RED), BR(TUI_YELLOW), BR(TUI_GREEN),
                                   BR(TUI_CYAN), BR(TUI_MAGENTA), BR(TUI_BLUE) };
    static const char BIT[] = "*+o.x*";
    for (int burst = 0; burst < 2; burst++) {
        const int64_t bt = t + burst * 1300;
        const int b = (int)(bt / 2600);
        const float lt = (float)(bt % 2600) / 1000.0f;
        const int ox = r.x + 6 + (int)(noise(b, 11 + burst) % (uint32_t)(r.w > 12 ? r.w - 12 : 1));
        const int oy = r.y + 3 + (int)(noise(b, 13 + burst) % (uint32_t)(r.h > 8 ? r.h / 2 : 1));
        for (int i = 0; i < 30; i++) {
            const float a = (float)(noise(i, b * 7 + burst) % 628) / 100.0f;
            const float v = 5.0f + (float)(noise(i, b * 3 + burst) % 9);
            const int x = ox + (int)lroundf(cosf(a) * v * lt * 2.0f);
            const int y = oy + (int)lroundf(sinf(a) * v * lt + 5.0f * lt * lt);
            const uint8_t at = lt > 1.8f ? A(PAL[i % 6] & 7, TUI_BLACK) : A(PAL[i % 6], TUI_BLACK);
            put(sf, r, x, y, BIT[i % 6], at);
        }
    }
}

/* Static: something failed. */
static void bg_glitch(tui_surface *sf, tui_rect r, int64_t t)
{
    const int slice = (int)(t / 110);
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) {
            const uint32_t h = noise(y * 389 + x, slice);
            if ((h & 255) > 5) continue;
            put(sf, r, r.x + x, r.y + y, (h >> 9) & 1 ? 'x' : HEX[(h >> 12) & 15],
                (h >> 10) & 1 ? A(BR(TUI_RED), TUI_BLACK) : A(TUI_RED, TUI_BLACK));
        }
}

static void background(tui_surface *sf, tui_rect r, long st, int bx, int by, int cx, int cy, int64_t t)
{
    switch (st) {
    case LS_OTA_ST_WIFI:        bg_stars(sf, r, t); bg_rings(sf, r, bx, by, t); break;
    case LS_OTA_ST_CHECKING:    bg_stars(sf, r, t); bg_orbit(sf, r, cx, cy, t); break;
    case LS_OTA_ST_AVAILABLE:   bg_stars(sf, r, t); bg_sparkle(sf, r, cx, cy, t); break;
    case LS_OTA_ST_DOWNLOADING: {
        bg_rain(sf, r, s_frame);
        break;
    }
    case LS_OTA_ST_VERIFYING:   bg_bands(sf, r, t); break;
    case LS_OTA_ST_RESTARTING:  bg_dissolve(sf, r, t); break;
    case LS_OTA_ST_UPDATED:     bg_stars(sf, r, t); bg_confetti(sf, r, t); break;
    case LS_OTA_ST_FAILED:      bg_glitch(sf, r, t); break;
    default:                    bg_stars(sf, r, t); break;
    }
}

/* ---------------------------------------------------------------- board -- */

enum { BOARD_W = 18, BOARD_H = 7 };

/* The board's own screen, sixteen cells by four, saying what the update is
   doing to it. */
static void board_screen(tui_surface *sf, tui_rect clip, int x, int y, long st, int pct, int64_t t)
{
    const int W = BOARD_W - 2, H = BOARD_H - 3;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            put(sf, clip, x + c, y + r, LS_TUI_SHADE_25, A(TUI_BLUE, TUI_BLACK));

    if (st == LS_OTA_ST_DOWNLOADING) {
        /* Filling from the bottom in half rows, warmer as it fills, with a
           ripple running along the top. */
        const int level = pct * H * 2 / 100;
        const int wave = (int)(t / 90) % W;
        for (int r = 0; r < H; r++) {
            const int lo = (H - 1 - r) * 2, hi = lo + 1;
            const uint8_t at = r == H - 1 ? A(TUI_RED, TUI_BLACK) : r == H - 2 ? A(BR(TUI_RED), TUI_BLACK)
                             : r == 1 ? A(TUI_YELLOW, TUI_BLACK) : A(BR(TUI_YELLOW), TUI_BLACK);
            for (int c = 0; c < W; c++) {
                const bool crest = (c + wave) % 5 == 0;
                char ch = 0;
                if (level > hi) ch = LS_TUI_BLOCK_FULL;
                else if (level > lo) ch = crest ? LS_TUI_QUAD(0, 1, 1, 1) : LS_TUI_BLOCK_LOWER;
                if (ch) put(sf, clip, x + c, y + r, ch, at);
            }
        }
    } else if (st == LS_OTA_ST_VERIFYING) {
        const int sub = (int)(t / 70) % (H * 3);
        for (int c = 0; c < W; c++) {
            for (int r = 0; r < H; r++)
                put(sf, clip, x + c, y + r, HEX[noise(c * 7 + r, (int)(t / 300)) % 16], A(TUI_CYAN, TUI_BLACK));
            put(sf, clip, x + c, y + sub / 3, LS_TUI_SEXT(3 << ((sub % 3) * 2)), A(BR(TUI_CYAN), TUI_BLACK));
        }
    } else if (st == LS_OTA_ST_RESTARTING) {
        /* the old picture collapsing to a line, then gone */
        const int p = (int)(t % 1000) / 250;
        for (int r = 0; r < H; r++)
            for (int c = 0; c < W; c++) {
                const bool lit = p == 0 || (p == 1 && (r == 1 || r == 2)) || (p == 2 && r == 2 && c > 3 && c < W - 4);
                if (lit) put(sf, clip, x + c, y + r, LS_TUI_BLOCK_FULL, A(BR(TUI_WHITE), TUI_BLACK));
            }
    } else if (st == LS_OTA_ST_UPDATED || st == LS_OTA_ST_CURRENT) {
        const int sweep = (int)(t / 60) % (W + H);
        for (int r = 0; r < H; r++)
            for (int c = 0; c < W; c++)
                put(sf, clip, x + c, y + r, LS_TUI_SHADE_50,
                    st == LS_OTA_ST_UPDATED && c + r == sweep ? A(BR(TUI_WHITE), TUI_BLACK)
                                                              : A(st == LS_OTA_ST_UPDATED ? BR(TUI_GREEN) : TUI_GREEN, TUI_BLACK));
        tui_put_str(sf, clip, x + W / 2 - 2, y + H / 2, " OK ", A(TUI_BLACK, BR(TUI_GREEN)));
    } else if (st == LS_OTA_ST_FAILED) {
        for (int r = 0; r < H; r++)
            for (int c = 0; c < W; c++)
                put(sf, clip, x + c, y + r, (noise(c + r * W, (int)(t / 150)) & 7) ? LS_TUI_SHADE_50 : 'x',
                    A(TUI_RED, TUI_BLACK));
        tui_put_str(sf, clip, x + W / 2 - 2, y + H / 2, " !! ", A(BR(TUI_WHITE), TUI_RED));
    } else if (st == LS_OTA_ST_CHECKING || st == LS_OTA_ST_WIFI || st == LS_OTA_ST_AVAILABLE) {
        /* a prompt, as if the board were typing */
        const char *line = st == LS_OTA_ST_WIFI ? "> wifi" : st == LS_OTA_ST_CHECKING ? "> fetch" : "> new!";
        const int shown = (int)((t / 120) % 16);
        char part[16];
        snprintf(part, sizeof(part), "%.*s", shown < (int)strlen(line) ? shown : (int)strlen(line), line);
        tui_put_str(sf, clip, x + 1, y + 1, part, A(st == LS_OTA_ST_AVAILABLE ? BR(TUI_MAGENTA) : BR(TUI_YELLOW), TUI_BLACK));
        if ((t / 400) & 1)
            put(sf, clip, x + 1 + (int)strlen(part), y + 1, LS_TUI_BLOCK_FULL, A(BR(TUI_YELLOW), TUI_BLACK));
    } else {
        tui_put_str(sf, clip, x + W / 2 - 1, y + H / 2, "P4", A(BR(TUI_CYAN), TUI_BLACK));
    }
}

static void board(tui_surface *sf, tui_rect clip, int x, int y, long st, int pct, const char *ver, int64_t t)
{
    const uint8_t hue = hue_of(st);
    fill(sf, clip, tui_rect_make(x, y, BOARD_W, BOARD_H + 2), ' ', A(TUI_WHITE, TUI_BLACK));
    if (y + BOARD_H - 1 <= clip.y + clip.h && x + BOARD_W <= clip.x + clip.w)
        ls_panel_box(sf, tui_rect_make(x, y, BOARD_W, BOARD_H - 1), NULL, hue);
    board_screen(sf, clip, x + 1, y + 1, st, pct, t);
    tui_put_str(sf, clip, x + 3, y + BOARD_H - 1, "o          o", A(hue, TUI_BLACK));
    char v[32], line[48];
    release_of(ver, v, sizeof(v));
    snprintf(line, sizeof(line), "THIS BOARD %s", v);
    tui_put_str(sf, clip, x + (BOARD_W - (int)strlen(line)) / 2, y + BOARD_H + 1, line, A(BR(TUI_WHITE), TUI_BLACK));
}

/* ---------------------------------------------------------------- cloud -- */

enum { CLOUD_W = 22, CLOUD_H = 5 };

static void cloud(tui_surface *sf, tui_rect clip, int x, int y, long st, const char *offer, int64_t t)
{
    static const char *const SHAPE[CLOUD_H - 1] = {
        "        .-~~~-.       ",
        "   .- (         ) -.  ",
        "  (       OTA       ) ",
        "   `-.___________.-'  ",
    };
    fill(sf, clip, tui_rect_make(x, y, CLOUD_W, CLOUD_H + 1), ' ', A(TUI_WHITE, TUI_BLACK));
    /* the outline shimmers: one band of brighter colour going round */
    const int band = (int)(t / 80) % (CLOUD_W + 8);
    for (int r = 0; r < CLOUD_H - 1; r++)
        for (int c = 0; SHAPE[r][c]; c++) {
            const char ch = SHAPE[r][c];
            if (ch == ' ') continue;
            uint8_t at = st == LS_OTA_ST_FAILED ? A(TUI_RED, TUI_BLACK)
                       : st == LS_OTA_ST_AVAILABLE ? A(BR(TUI_MAGENTA), TUI_BLACK) : A(TUI_CYAN, TUI_BLACK);
            if (abs(c - band) <= 1 && st != LS_OTA_ST_FAILED) at = A(BR(TUI_WHITE), TUI_BLACK);
            if (ch >= 'A' && ch <= 'Z') at = A(BR(TUI_WHITE), TUI_BLACK);
            put(sf, clip, x + c, y + r, ch, at);
        }
    char v[32], line[48];
    if (offer[0]) release_of(offer, v, sizeof(v));
    else snprintf(v, sizeof(v), "%s", st == LS_OTA_ST_FAILED ? "?" : "...");
    snprintf(line, sizeof(line), "SERVER %s", v);
    tui_put_str(sf, clip, x + (CLOUD_W - (int)strlen(line)) / 2, y + CLOUD_H - 1, line,
                st == LS_OTA_ST_AVAILABLE ? A(BR(TUI_MAGENTA), TUI_BLACK) : A(BR(TUI_WHITE), TUI_BLACK));
}

/* ----------------------------------------------------------------- pipe -- */

/* The connection, `len` cells from (x, y) going `dx`, `dy` (one of them 0):
   three lanes wide, the bytes going toward the board. */
static void pipe(tui_surface *sf, tui_rect clip, int x, int y, int dx, int dy, int len, long st, int64_t t,
                 uint32_t frame)
{
    if (len < 2) return;
    const int px = dy ? 1 : 0, py = dx ? 1 : 0;           /* across the pipe */
    for (int i = 0; i < len; i++)
        for (int lane = -2; lane <= 2; lane++) {
            const int cx = x + dx * i + px * lane, cy = y + dy * i + py * lane;
            if (lane == -2 || lane == 2) {                   /* the walls */
                const uint8_t wall = st == LS_OTA_ST_FAILED ? A(TUI_RED, TUI_BLACK) : A(TUI_BLUE, TUI_BLACK);
                put(sf, clip, cx, cy, dy ? '|' : '-', wall);
                continue;
            }
            char ch = ' ';
            uint8_t at = A(TUI_WHITE, TUI_BLACK);
            /* flows from the server end, one cell a frame while downloading */
            const int k = i - (st == LS_OTA_ST_DOWNLOADING ? (int)frame : 0) + lane * 3;
            switch (st) {
            case LS_OTA_ST_DOWNLOADING: {
                const uint32_t h = noise(lane * 977 + (k >> 1), 3);
                ch = HEX[h % 16];
                at = (k & 7) == 0 ? A(BR(TUI_WHITE), TUI_BLACK) : (h & 1) ? A(BR(TUI_YELLOW), TUI_BLACK) : A(TUI_YELLOW, TUI_BLACK);
                break;
            }
            case LS_OTA_ST_CHECKING: {
                const int span = len * 2 - 2, p = (int)(t / 60) % (span > 0 ? span : 1);
                const int at_i = p < len ? p : span - p;
                if (lane == 0 && i == at_i) { ch = 'o'; at = A(BR(TUI_YELLOW), TUI_BLACK); }
                else if (lane == 0) { ch = '.'; at = LS_ATTR_FAINT; }
                break;
            }
            case LS_OTA_ST_AVAILABLE:
                if (lane == 0 && ((i + (int)(t / 200)) % 4) == 0) {
                    ch = dy ? 'v' : '<';
                    at = A(BR(TUI_MAGENTA), TUI_BLACK);
                }
                break;
            case LS_OTA_ST_VERIFYING:
            case LS_OTA_ST_RESTARTING:
                ch = LS_TUI_SHADE_50;
                at = A(TUI_CYAN, TUI_BLACK);
                break;
            case LS_OTA_ST_CURRENT:
            case LS_OTA_ST_UPDATED:
                ch = LS_TUI_SHADE_50;
                at = (i == (int)(t / 50) % (len + 6)) ? A(BR(TUI_WHITE), TUI_BLACK) : A(BR(TUI_GREEN), TUI_BLACK);
                break;
            case LS_OTA_ST_FAILED:
                if (abs(i - len / 2) > 1) { ch = LS_TUI_SHADE_25; at = A(TUI_RED, TUI_BLACK); }
                else if (lane == 0) { ch = 'x'; at = A(BR(TUI_RED), TUI_BLACK); }
                break;
            default:
                if (lane == 0 && (i & 1)) { ch = '.'; at = LS_ATTR_FAINT; }
            }
            if (ch != ' ') put(sf, clip, cx, cy, ch, at);
        }
}

/* ---------------------------------------------------------------- steps -- */

static void steps(tui_surface *sf, tui_rect clip, int y, long st, long got, long size, int64_t t)
{
    static const char *const LONG_[] = { "CHECK", "DOWNLOAD", "VERIFY", "INSTALL", "RESTART" };
    static const char *const MID_[] = { "CHECK", "GET", "VERIFY", "INSTALL", "BOOT" };
    static const char *const SHORT_[] = { "CHK", "GET", "VER", "INS", "RST" };
    const char *const *name = clip.w >= 55 ? LONG_ : clip.w >= 47 ? MID_ : SHORT_;

    int active = -1, done = 0, failed = -1;
    switch (st) {
    case LS_OTA_ST_WIFI: case LS_OTA_ST_CHECKING: active = 0; break;
    case LS_OTA_ST_CURRENT:     done = 1; break;
    case LS_OTA_ST_AVAILABLE:   done = 1; break;
    case LS_OTA_ST_DOWNLOADING: done = 1; active = 1; break;
    case LS_OTA_ST_VERIFYING:   done = 2; active = 2; break;
    case LS_OTA_ST_RESTARTING:  done = 4; active = 4; break;
    case LS_OTA_ST_UPDATED:     done = 5; break;
    case LS_OTA_ST_FAILED:
        failed = got <= 0 ? 0 : got < size ? 1 : 2;
        done = failed;
        break;
    default: break;
    }
    int width = 0;
    for (int i = 0; i < 5; i++) width += (int)strlen(name[i]) + 2 + (i ? 3 : 0);
    fill(sf, clip, tui_rect_make(clip.x, y, clip.w, 1), ' ', A(TUI_WHITE, TUI_BLACK));
    int x = clip.x + (clip.w - width) / 2;
    static const char SPIN[] = "|/-\\";
    for (int i = 0; i < 5; i++) {
        if (i) {
            /* the arrows between steps light up as the work passes them */
            const bool lit = i <= done;
            const bool moving = i == active && ((t / 150) & 1);
            tui_put_str(sf, clip, x, y, moving ? "=> " : " > ", lit || moving ? A(BR(TUI_GREEN), TUI_BLACK) : LS_ATTR_FAINT);
            x += 3;
        }
        char chip[16];
        snprintf(chip, sizeof(chip), " %s ", name[i]);
        uint8_t at = LS_ATTR_FAINT;
        if (i == failed)      at = A(BR(TUI_WHITE), TUI_RED);
        else if (i == active) at = A(TUI_BLACK, (t / 300) & 1 ? BR(TUI_YELLOW) : TUI_YELLOW);
        else if (i < done)    at = A(TUI_BLACK, TUI_GREEN);
        tui_put_str(sf, clip, x, y, chip, at);
        if (i == active)
            put(sf, clip, x + (int)strlen(chip) - 1, y, SPIN[(t / 120) % 4], A(TUI_BLACK, BR(TUI_YELLOW)));
        x += (int)strlen(chip);
    }
}

/* ----------------------------------------------------------------- hero -- */

/* Where there is room: the percentage in figures as tall as a thumb while it
   downloads, otherwise one word in block letters with colour running down.
   Returns where the word went, so a tap on READY can install; the figures
   are not a button and come back empty. */
enum { HERO_H = 12 };

static tui_rect hero(tui_surface *sf, tui_rect clip, int y, long st, long got, long size,
                     bool retry, int64_t t)
{
    static const uint8_t HOT[] = { BR(TUI_YELLOW), BR(TUI_WHITE), BR(TUI_YELLOW), TUI_YELLOW, BR(TUI_RED), TUI_YELLOW };
    fill(sf, clip, tui_rect_make(clip.x, y, clip.w, HERO_H), ' ', A(TUI_WHITE, TUI_BLACK));
    if (st == LS_OTA_ST_DOWNLOADING && size > 0) {
        char num[8];
        snprintf(num, sizeof(num), "%d", (int)((int64_t)got * 100 / size));
        const int n = (int)strlen(num), cw = 10, total = n * cw + 3;
        const int x = clip.x + (clip.w - total) / 2;
        for (int i = 0; i < n; i++)
            ls_glyph_draw(sf, tui_rect_make(x + i * cw, y, cw, HERO_H), num[i],
                          A(HOT[((int)(t / 140) + i) % 6], TUI_BLACK));
        tui_put_str(sf, clip, x + n * cw, y + HERO_H - 3, "%", A(BR(TUI_YELLOW), TUI_BLACK));
        return tui_rect_make(0, 0, 0, 0);
    }
    const char *word = st == LS_OTA_ST_AVAILABLE ? (retry ? "RETRY" : "READY") : st == LS_OTA_ST_CURRENT ? "LATEST"
                     : st == LS_OTA_ST_UPDATED ? "UPDATED" : st == LS_OTA_ST_FAILED ? "FAILED"
                     : st == LS_OTA_ST_VERIFYING ? "VERIFY" : st == LS_OTA_ST_RESTARTING ? (ready_to_restart(st) ? "DONE" : "REBOOT")
                     : st == LS_OTA_ST_CHECKING ? "CHECK" : st == LS_OTA_ST_WIFI ? "WIFI" : "UPDATE";
    /* The same figures as the percentage: six cells wide where the word
       fits at that, three where it does not, ten rows tall either way. */
    const int n = (int)strlen(word);
    int pitch = 7, rw = 10, inset = 2;
    if (n * pitch - 1 > clip.w) { pitch = 4; rw = 5; inset = 1; }
    if (n * pitch - 1 > clip.w) return tui_rect_make(0, 0, 0, 0);
    const int x0 = clip.x + (clip.w - (n * pitch - 1)) / 2;
    const uint8_t base = hue_of(st);
    const int shine = (int)(t / 130) % (n + 5);
    static const uint8_t RAIN[] = { BR(TUI_RED), BR(TUI_YELLOW), BR(TUI_GREEN), BR(TUI_CYAN), BR(TUI_BLUE), BR(TUI_MAGENTA) };
    for (int i = 0; i < n; i++) {
        uint8_t fg = i == shine ? BR(TUI_WHITE) : (i == shine - 1 || i == shine + 1) ? BR(base & 7) : base;
        if (st == LS_OTA_ST_UPDATED) fg = RAIN[(i + (int)(t / 160)) % 6];
        if (st == LS_OTA_ST_FAILED && (noise(i, (int)(t / 90)) & 7) == 0) fg = BR(TUI_WHITE);
        ls_glyph_draw(sf, tui_rect_make(x0 + i * pitch - inset, y, rw, HERO_H), word[i], A(fg, TUI_BLACK));
    }
    return tui_rect_make(x0 - 1, y, n * pitch + 1, HERO_H);
}

/* --------------------------------------------------------------- button -- */

/* The one thing to press now, as big as the room allows. Eight rows or more
   get a pill in the state's colour with the word on it in block letters and
   a line under it; fewer get one row of text. Either way it breathes (the
   edges swell from half rows to whole ones and the colour brightens), a
   glare crosses it, and chevrons march in from both sides when they fit.
   Returns the rect a tap counts in. */
static tui_rect cta(tui_surface *sf, tui_rect clip, int y, int room, const char *label,
                    const char *sub, uint8_t hue, int64_t t)
{
    const bool big = room >= 8;
    const int h = big ? 8 : 3, n = (int)strlen(label), ns = (int)strlen(sub);
    const int text_w = big ? n * 4 - 1 : n + 3 + ns;
    int w = text_w + 4;
    if (big && ns + 4 > w) w = ns + 4;
    const int chevrons = w + 14 <= clip.w ? 2 : w + 8 <= clip.w ? 1 : 0;      /* a side */
    if (w > clip.w) w = clip.w;
    const int x = clip.x + (clip.w - w) / 2;

    /* the breath: half rows and the plain colour, then whole rows and the
       bright one, a little over once a second */
    const int breath = ls_motion_phase(4, 1400);
    const bool swell = breath == 1 || breath == 2;
    const uint8_t body = swell ? BR(hue & 7) : (uint8_t)(hue & 7);
    const int glare = (int)(t / 28) % (w + h * 2 + 24) - h * 2;

    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            const int cx = x + c, cy = y + r;
            const bool top = r == 0, bottom = r == h - 1, left = c == 0, right = c == w - 1;
            if (top || bottom) {
                /* the rim: the inner half row, or all of it while it swells,
                   with the outside corner left off so the ends stay round */
                char ch;
                if (top && left)          ch = swell ? LS_TUI_QUAD(0, 1, 1, 1) : LS_TUI_QUAD(0, 0, 0, 1);
                else if (top && right)    ch = swell ? LS_TUI_QUAD(1, 0, 1, 1) : LS_TUI_QUAD(0, 0, 1, 0);
                else if (left)            ch = swell ? LS_TUI_QUAD(1, 1, 0, 1) : LS_TUI_QUAD(0, 1, 0, 0);
                else if (right)           ch = swell ? LS_TUI_QUAD(1, 1, 1, 0) : LS_TUI_QUAD(1, 0, 0, 0);
                else                      ch = swell ? LS_TUI_BLOCK_FULL : top ? LS_TUI_BLOCK_LOWER : LS_TUI_BLOCK_UPPER;
                put(sf, clip, cx, cy, ch, A(body, TUI_BLACK));
                continue;
            }
            const bool lit = c - r * 2 >= glare && c - r * 2 < glare + 3;
            put(sf, clip, cx, cy, lit ? LS_TUI_SHADE_50 : ' ', A(BR(TUI_WHITE), body));
        }

    char l[64];
    if (big) {
        const int x0 = x + (w - text_w) / 2;
        for (int i = 0; i < n; i++)
            ls_glyph_draw(sf, tui_rect_make(x0 + i * 4 - 1, y + 1, 5, 6), label[i], A(BR(TUI_WHITE), body));
        snprintf(l, sizeof(l), "%.*s", w - 2, sub);
        tui_put_str(sf, clip, x + (w - (int)strlen(l)) / 2, y + h - 2, l, A(TUI_BLACK, body));
    } else {
        char full[64];
        snprintf(full, sizeof(full), "%s   %s", label, sub);
        snprintf(l, sizeof(l), "%.*s", w - 2 > 0 ? w - 2 : 0, full);
        const int lx = x + (w - (int)strlen(l)) / 2;
        tui_put_str(sf, clip, lx, y + 1, l, A(TUI_BLACK, body));
        snprintf(full, sizeof(full), "%.*s", (int)strlen(l), label);
        tui_put_str(sf, clip, lx, y + 1, full, A(BR(TUI_WHITE), body));
    }

    if (chevrons) {
        /* the lit chevron moving in toward the button; a lone one blinks */
        const int mid = y + h / 2 - (big ? 1 : 0), tall = big ? 1 : 0;
        const int lit = chevrons - 1 - ls_motion_phase(2, 500);
        for (int k = 0; k < chevrons; k++) {
            const uint8_t at = k == lit ? A(BR(TUI_WHITE), TUI_BLACK) : A(hue, TUI_BLACK);
            const int lx = x - 3 - k * 3, rx = x + w + 1 + k * 3;
            if (tall) {
                put(sf, clip, lx, mid - 1, '\\', at);
                put(sf, clip, lx + 1, mid, '>', at);
                put(sf, clip, lx, mid + 1, '/', at);
                put(sf, clip, rx + 1, mid - 1, '/', at);
                put(sf, clip, rx, mid, '<', at);
                put(sf, clip, rx + 1, mid + 1, '\\', at);
            } else {
                put(sf, clip, lx + 1, mid, '>', at);
                put(sf, clip, rx, mid, '<', at);
            }
        }
    }
    return tui_rect_make(x, y, w, h);
}

/* ------------------------------------------------------------- progress -- */

static void progress(tui_surface *sf, tui_rect clip, int y, int rows, long st, long got, long size, int64_t t)
{
    const int w = clip.w;
    int halves = 0;
    if (st == LS_OTA_ST_DOWNLOADING && size > 0) halves = (int)((int64_t)got * w * 2 / size);
    else if (st == LS_OTA_ST_VERIFYING || st == LS_OTA_ST_RESTARTING || st == LS_OTA_ST_UPDATED) halves = w * 2;
    else if (st == LS_OTA_ST_FAILED && size > 0 && got > 0) halves = (int)((int64_t)got * w * 2 / size);

    fill(sf, clip, tui_rect_make(clip.x, y, w, rows + 1), ' ', A(TUI_WHITE, TUI_BLACK));
    const int shine = halves > 4 ? (int)(t / 25) % (halves / 2 + 8) : -1;
    for (int r = 0; r < rows; r++)
        for (int i = 0; i < w; i++) {
            /* red through yellow to green along the bar; one colour for the
               stages after the download */
            uint8_t fill = i < w / 3 ? BR(TUI_RED) : i < w * 2 / 3 ? BR(TUI_YELLOW) : BR(TUI_GREEN);
            if (st == LS_OTA_ST_VERIFYING || st == LS_OTA_ST_RESTARTING) fill = BR(TUI_CYAN);
            if (st == LS_OTA_ST_UPDATED) fill = BR(TUI_GREEN);
            if (st == LS_OTA_ST_FAILED) fill = TUI_RED;
            char ch = LS_TUI_SHADE_25;
            uint8_t at = LS_ATTR_FAINT;
            if (halves >= (i + 1) * 2) {
                ch = LS_TUI_BLOCK_FULL;
                at = abs(i - (shine - r)) <= 1 ? A(BR(TUI_WHITE), TUI_BLACK) : A(fill, TUI_BLACK);
            } else if (halves == i * 2 + 1) {
                ch = LS_TUI_BLOCK_LEFT;
                at = A(fill, TUI_BLACK);
            }
            put(sf, clip, clip.x + i, y + r, ch, at);
        }
    /* sparks off the leading edge while it moves */
    if (st == LS_OTA_ST_DOWNLOADING && halves > 0 && halves < w * 2) {
        const int ex = clip.x + halves / 2;
        for (int s = 0; s < 4; s++) {
            const uint32_t h = noise(s, (int)(t / 90));
            put(sf, clip, ex + (int)(h % 3), y - 1 + (int)((h >> 4) % (unsigned)(rows + 2)),
                (h >> 8) & 1 ? '*' : '.', A(BR((h >> 9) & 1 ? TUI_WHITE : TUI_YELLOW), TUI_BLACK));
        }
    }

    char line[96] = "";
    if (st == LS_OTA_ST_DOWNLOADING && size > 0) {
        const int pct = (int)((int64_t)got * 100 / size);
        if (s_rate > 1)
            snprintf(line, sizeof(line), "%.2f / %.2f MB   %d%%   %.0f KB/s   %d s left",
                     got / 1048576.0, size / 1048576.0, pct, s_rate / 1024.0, (int)((size - got) / s_rate));
        else
            snprintf(line, sizeof(line), "%.2f / %.2f MB   %d%%", got / 1048576.0, size / 1048576.0, pct);
    } else if (st == LS_OTA_ST_VERIFYING) {
        snprintf(line, sizeof(line), "%.2f MB in: SHA-256 and the image check", size / 1048576.0);
    } else if (st == LS_OTA_ST_RESTARTING) {
        snprintf(line, sizeof(line), "written to the other slot; restarting into it");
    } else if (st == LS_OTA_ST_UPDATED) {
        snprintf(line, sizeof(line), "running the new build");
    }
    if (line[0]) centre(sf, clip, y + rows, line, A(BR(TUI_WHITE), TUI_BLACK));
}

/* -------------------------------------------------------------- details -- */

typedef struct { const char *p; int n; } span_t;

/* `s` broken into lines of at most `room` columns, at spaces where one is
   near enough. Returns how many, up to `cap`. */
static int wrap(const char *s, int room, span_t *out, int cap)
{
    int k = 0;
    while (*s && k < cap) {
        int n = (int)strlen(s);
        if (n > room) {
            n = room;
            while (n > room / 2 && s[n] != ' ') n--;
            if (s[n] != ' ') n = room;
        }
        out[k].p = s;
        out[k].n = n;
        k++;
        s += n;
        while (*s == ' ') s++;
    }
    return k;
}

enum { KEY_W = 12 };                        /* "running", "server" and the rest */

static int detail(tui_surface *sf, tui_rect clip, int y, const char *key, const char *val, uint8_t at)
{
    if (y >= clip.y + clip.h) return y;
    tui_put_str(sf, clip, clip.x, y, key, LS_ATTR_DIM);
    const int room = clip.w - KEY_W;
    if (room < 4) return y + 1;
    span_t line[8];
    const int n = wrap(val, room, line, 8);
    for (int i = 0; i < n && y < clip.y + clip.h; i++, y++) {
        char row[160];
        snprintf(row, sizeof(row), "%.*s", line[i].n, line[i].p);
        tui_put_str(sf, clip, clip.x + KEY_W, y, row, at);
    }
    return n ? y : y + 1;
}

static void details(tui_surface *sf, tui_rect r, long st, long trial, const char *run,
                    const char *offer, const char *failed, int64_t t)
{
    if (r.h <= 0) return;
    fill(sf, r, r, ' ', A(TUI_WHITE, TUI_BLACK));
    int y = r.y;
    y = detail(sf, r, y, "running", run[0] ? run : "?", A(BR(TUI_WHITE), TUI_BLACK));
    y = detail(sf, r, y, "server", offer[0] ? offer : st == LS_OTA_ST_CURRENT ? "nothing published yet" : "not checked yet",
               st == LS_OTA_ST_AVAILABLE ? A(BR(TUI_MAGENTA), TUI_BLACK) : A(TUI_WHITE, TUI_BLACK));
    if (failed[0]) {
        /* the bootloader put this build back after the last one would not
           start */
        char v[32], line[80];
        release_of(failed, v, sizeof(v));
        snprintf(line, sizeof(line), "%s did not start, so this build came back", v);
        y = detail(sf, r, y, "last try", line, A(BR(TUI_RED), TUI_BLACK));
    }
    if (st == LS_OTA_ST_UPDATED) {
        char line[48];
        if (trial > 0) {
            /* a fuse burning down to "kept" */
            char fuse[24];
            const int n = trial > 20 ? 20 : (int)trial;
            for (int i = 0; i < n; i++) fuse[i] = i == n - 1 && ((t / 200) & 1) ? '*' : '=';
            fuse[n] = 0;
            snprintf(line, sizeof(line), "%s %ld s", fuse, trial);
        } else {
            snprintf(line, sizeof(line), "kept");
        }
        detail(sf, r, y, "trial", line, A(BR(TUI_GREEN), TUI_BLACK));
    }
}

/* ----------------------------------------------------------------- page -- */

/* Whether a press does anything: not while a job runs, and not while a new
   build is proving itself, where a check would hide the trial. */
static bool pressable(long st, long trial)
{
    return !working(st) && !(st == LS_OTA_ST_UPDATED && trial > 0);
}

/* A press: check, or install what the check found. */
static void step(void)
{
    long st = -1, trial = 0;
    if (!get_int("ota.stage", &st)) return;
    get_int("ota.trial", &trial);
    if (!pressable(st, trial)) return;
    ls_args_t in = { 0 };
    ls_val_t out;
    ls_action_call("ota.step", &in, &out, LS_CAP_STORE | LS_CAP_POWER);
}

void ls_update_view_enter(void)
{
    s_got_prev = -1;
    s_rate = 0;
    s_cta_on = s_hero_on = false;
    long st = LS_OTA_ST_IDLE;
    if (!get_int("ota.stage", &st)) return;
    /* Opening the page is asking whether there is an update, unless one is
       already found or under way, or this boot is a new build proving
       itself. */
    if (st != LS_OTA_ST_AVAILABLE && st != LS_OTA_ST_RESTARTING) step();
}

void ls_update_view_draw(tui_surface *sf, tui_rect a)
{
    long st = -1;
    if (!get_int("ota.stage", &st)) {
        ls_panel_notice(sf, a, "UPDATE", "Updates over WiFi are not in this build", "ESC goes back");
        return;
    }
    if (a.w < 30 || a.h < 18) {
        ls_panel_notice(sf, a, "UPDATE", "Enlarge the pane", "ESC goes back");
        return;
    }
    const int64_t t = esp_timer_get_time() / 1000;
    long got = 0, size = 0, trial = 0;
    get_int("ota.got", &got);
    get_int("ota.size", &size);
    get_int("ota.trial", &trial);
    char run[48], offer[48], failed[48], state[64];
    get_text("ota.running", run, sizeof(run));
    get_text("ota.build", offer, sizeof(offer));
    get_text("ota.failed", failed, sizeof(failed));
    get_text("ota.state", state, sizeof(state));
    s_cta_on = s_hero_on = false;

    if (st == LS_OTA_ST_DOWNLOADING) {
        const int64_t now = esp_timer_get_time();
        if (s_got_prev < 0 || got < s_got_prev) { s_got_prev = got; s_got_us = now; s_rate = 0; }
        else if (now - s_got_us >= 500000) {
            const float inst = (float)(got - s_got_prev) * 1e6f / (float)(now - s_got_us);
            s_rate = s_rate > 0 ? s_rate * 0.7f + inst * 0.3f : inst;
            s_got_prev = got;
            s_got_us = now;
        }
        s_frame++;                       /* once a drawn frame of the page */
    }

    const bool busy = working(st);
    const bool ready = ready_to_restart(st);
    const long left = ready ? restart_left() : 0;
    /* what the scene shows: a build that is written and waiting looks like
       one that is up, and only the restart itself dissolves it */
    const long vis = ready ? LS_OTA_ST_UPDATED : st;
    const bool tappable = st == LS_OTA_ST_AVAILABLE || ready;
    const bool retry = st == LS_OTA_ST_AVAILABLE && failed[0] && !strcmp(failed, offer);
    char sub[24], csub[48], bsub[16];
    release_of(offer, sub, sizeof(sub));
    static const char *const DOING[LS_OTA_ST_RESTARTING + 1] = {
        [LS_OTA_ST_WIFI] = "wifi", [LS_OTA_ST_CHECKING] = "asking",
        [LS_OTA_ST_DOWNLOADING] = "loading", [LS_OTA_ST_VERIFYING] = "checks",
        [LS_OTA_ST_RESTARTING] = "reboot",
    };
    /* the one thing a press does now, if anything */
    const char *press = !pressable(st, trial) ? NULL
                      : ready ? "RESTART"
                      : st == LS_OTA_ST_AVAILABLE ? "INSTALL" : st == LS_OTA_ST_FAILED ? "RETRY" : "CHECK";
    snprintf(bsub, sizeof(bsub), "%ld s", left);
    if (ready)
        snprintf(csub, sizeof(csub), "restarts by itself in %ld s", left);
    else if (retry)
        snprintf(csub, sizeof(csub), "%s again", sub);
    else if (st == LS_OTA_ST_AVAILABLE)
        snprintf(csub, sizeof(csub), "%s  %.1f MB", sub, size / 1048576.0);
    else
        snprintf(csub, sizeof(csub), "ask the server again");
    const ls_btn_t btn[] = {
        { "BACK", "HOME", 0, false, false },
        { press ? press : "WAIT", busy ? DOING[st] : !press ? "trial" : ready ? bsub : st == LS_OTA_ST_AVAILABLE ? sub : "again",
          0, tappable, !press },
    };
    ls_btn_bar(sf, tui_rect_make(a.x, a.y, a.w, 3), btn, 2, -1);

    const tui_rect body = tui_rect_make(a.x, a.y + 3, a.w, a.h - 3);
    const uint8_t hue = hue_of(st);
    ls_panel_box(sf, body, "SYSTEM UPDATE", hue);
    ls_motion_busy(sf, body, busy);
    const tui_rect in = tui_rect_make(body.x + 1, body.y + 1, body.w - 2, body.h - 2);
    const int pct = size > 0 ? (int)((int64_t)got * 100 / size) : 0;

    if (in.w >= 90 && in.h >= 17) {
        /* Landscape: the scene, steps and bar on the left; the figures and
           the details on the right. */
        const int lw = in.w * 55 / 100;
        const tui_rect L = tui_rect_make(in.x + 1, in.y, lw - 2, in.h - 1);
        const tui_rect R = tui_rect_make(in.x + lw + 1, in.y, in.w - lw - 2, in.h - 1);
        const int bx = L.x + 1, by = L.y + 1, cx = L.x + L.w - CLOUD_W - 1, cy = L.y + 1;
        background(sf, in, vis, bx + BOARD_W / 2, by + 3, cx + CLOUD_W / 2, cy + 2, t);
        board(sf, L, bx, by, vis, pct, run, t);
        cloud(sf, L, cx, cy, vis, offer, t);
        pipe(sf, L, cx - 1, by + 3, -1, 0, cx - 1 - (bx + BOARD_W), vis, t, s_frame);
        int y = L.y + BOARD_H + 3;
        steps(sf, L, y, st, got, size, t);
        if (press) {
            s_cta = cta(sf, L, y + 2, L.y + L.h - (y + 2), press, csub, hue, t);
            s_cta_on = true;
        } else {
            progress(sf, L, y + 2, 2, st, got, size, t);
        }
        s_hero = hero(sf, R, R.y, st, got, size, retry, t);
        s_hero_on = tappable && s_hero.w > 0;
        details(sf, tui_rect_make(R.x, R.y + HERO_H + 1, R.w, R.h - HERO_H - 1), st, trial, run, offer,
                failed, t);
    } else {
        /* Portrait: the server on top, the pipe pouring down into the board,
           then the figures, the steps and the bar. A shorter pane drops the
           figures, then the scene, and keeps the bar and the words. */
        const bool roomy = in.h >= 50, scene = in.h >= 26 && in.w >= CLOUD_W + 2;
        const int cx = in.x + (in.w - CLOUD_W) / 2, bx = in.x + (in.w - BOARD_W) / 2;
        const int cy = in.y + (roomy ? 1 : 0);
        /* the pipe takes whatever height the rest leaves: more to watch */
        int pipe_len = roomy ? in.h - 46 : 3;
        if (pipe_len < 4) pipe_len = roomy ? 4 : 3;
        if (pipe_len > 14) pipe_len = 14;
        const int by = cy + CLOUD_H + pipe_len;
        background(sf, in, vis, bx + BOARD_W / 2, scene ? by + 3 : in.y + in.h / 2,
                   cx + CLOUD_W / 2, scene ? cy + 2 : in.y + 2, t);
        int y = in.y + 1;
        if (scene) {
            cloud(sf, in, cx, cy, vis, offer, t);
            pipe(sf, in, in.x + in.w / 2, cy + CLOUD_H, 0, 1, pipe_len, vis, t, s_frame);
            board(sf, in, bx, by, vis, pct, run, t);
            y = by + BOARD_H + 3;
        }
        const int last = in.y + in.h - 1;                  /* the state line's row */
        if (y < last) steps(sf, in, y, st, got, size, t);
        y += 2;
        if (roomy) {
            s_hero = hero(sf, in, y, st, got, size, retry, t);
            s_hero_on = tappable && s_hero.w > 0;
            y += HERO_H + 1;
        }
        if (press) {
            /* the button where the bar would be: big when the details can
               still have a few rows under it */
            const int room = last - y;
            if (room >= 3) {
                s_cta = cta(sf, in, y, room >= 8 + 4 ? 8 : 3, press, csub, hue, t);
                s_cta_on = true;
                y += s_cta.h + 1;
            }
        } else {
            const int rows = roomy ? 3 : 1;
            if (y + rows < last)
                progress(sf, tui_rect_make(in.x + 2, in.y, in.w - 4, last - in.y), y, rows, st, got, size, t);
            y += rows + 2;
        }
        if (y < last)
            details(sf, tui_rect_make(in.x + 2, y, in.w - 4, last - y), st, trial, run, offer, failed, t);
    }

    /* the one line that says it all, at the bottom, on its own black */
    fill(sf, in, tui_rect_make(in.x, in.y + in.h - 1, in.w, 1), ' ', A(TUI_WHITE, TUI_BLACK));
    ls_safe_line(sf, in, in.y + in.h - 1, state, A(hue, TUI_BLACK));
}

static bool inside(bool on, tui_rect r, int col, int row)
{
    return on && col >= r.x && col < r.x + r.w && row >= r.y && row < r.y + r.h;
}

bool ls_update_view_key(ls_tk_t k, char ch)
{
    (void)ch;
    if (k != LS_TK_ENTER) return false;      /* ESC home, F-keys switch apps: the router's */
    step();
    return true;
}

bool ls_update_view_touch(int col, int row)
{
    const int i = ls_btn_hit(col, row);
    if (i == 0) return true;
    if (i == 1 || inside(s_cta_on, s_cta, col, row) || inside(s_hero_on, s_hero, col, row)) step();
    return false;
}
