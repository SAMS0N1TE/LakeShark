/* Wallpapers: the desktop's ground. Built-in ones in the house style
   (character patterns, the LakeShark mark, and the GPS sky, which moves only
   when the receiver reports), and pictures from the card.

   A picture is a region of image cells over the work area with one RGB565
   source, the same path MAP and the waterfalls use, so the cell feed carries
   it as picture cells now and as board pixels once the feed has a pixel
   record: the region and its source are already what such a record needs. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "esp_attr.h"
#include "../ls_braille.h"
#include "../ls_theme.h"
#include "ls_gps.h"

extern void ls_gps_get(ls_gps_state_t *out) __attribute__((weak));
EXT_RAM_BSS_ATTR static ls_gps_state_t s_sky;

enum { WP_PLAIN, WP_DOTS, WP_WAVES, WP_WEAVE, WP_CIRCUIT, WP_LOGO, WP_SKY, WP_COUNT };
static const char *const NAMES[WP_COUNT] = {
    "Plain", "Dots", "Waves", "Weave", "Circuit", "LakeShark", "GPS sky",
};

static uint32_t s_cur;
/* The picture for a file wallpaper, made for one area size. */
static uint16_t *s_px;
static int s_pw, s_ph, s_area_w, s_area_h, s_px_fit;
static uint32_t s_px_id, s_serial = 0x5A110000u;
/* A small copy of a picture for the Display Properties monitor. */
static uint16_t *s_pv;
static int s_pvw, s_pvh;
static uint32_t s_pv_id;
static bool s_failed;
static char s_files[8][40];
static int s_nfiles = -1;

/* Same choices, same cells: a picture (registered and glassed each frame) and
   the sky (moves when the receiver reports) are drawn every frame instead. */
bool dk_wall_still(void) { return !(s_cur & DK_WP_FILE) && s_cur != WP_SKY; }

int dk_wall_builtin_count(void) { return WP_COUNT; }
const char *dk_wall_builtin_name(int i) { return i >= 0 && i < WP_COUNT ? NAMES[i] : ""; }
uint32_t dk_wall_current(void) { return s_cur; }

static uint32_t name_id(const char *n)
{
    uint32_t h = 2166136261u;
    while (*n) h = (h ^ (uint8_t)*n++) * 16777619u;
    return DK_WP_FILE | (h & 0x7FFFFFFFu);
}

int dk_wall_files(const char **names, uint32_t *ids, int cap)
{
    s_nfiles = dk_sys_wallpaper_files(s_files, 8);
    int n = 0;
    for (int i = 0; i < s_nfiles && n < cap; i++, n++) {
        names[n] = s_files[i];
        ids[n] = name_id(s_files[i]);
    }
    return n;
}

static void drop_picture(void)
{
    if (s_px) dk_psram_free(s_px);
    s_px = NULL;
    s_px_id = 0;
}

void dk_wall_set(uint32_t id)
{
    if (!(id & DK_WP_FILE) && id >= WP_COUNT) id = WP_PLAIN;
    if (id != s_cur) {
        drop_picture();
        s_failed = false;
        dk_sys_save_wallpaper(id);
    }
    s_cur = id;
    ls_tui_invalidate();
}

/* ---------------------------------------------------------- patterns -- */

static uint8_t ground(void) { return dk_attr(R_DESK); }
static uint8_t ink(uint8_t fg) { return (uint8_t)((ground() & 0xF0) | (fg & 0x0F)); }
static bool bay(void) { return !dk_light_desk(); }

static void dots(tui_surface *sf, tui_rect a)
{
    const uint8_t at = ink(bay() ? TUI_BLUE : (TUI_WHITE | TUI_BRIGHT));
    for (int y = a.y; y < a.y + a.h; y++)
        for (int x = a.x + ((y & 1) ? 2 : 0); x < a.x + a.w; x += 4)
            ls_braille_put(sf, a, x, y, (int16_t)(0x2800 | 0x02), at);
}

static void waves(tui_surface *sf, tui_rect a)
{
    for (int y = a.y; y < a.y + a.h; y++) {
        if (y % 3 == 2) continue;
        const uint8_t at = ink(y % 3 == 0 ? (bay() ? TUI_BLUE : TUI_BLUE) : (bay() ? TUI_CYAN : (TUI_CYAN | TUI_BRIGHT)));
        for (int x = a.x; x < a.x + a.w; x++)
            if (((x + y * 3) % 6) < 3) tui_put_char(sf, a, x, y, '~', at);
    }
}

static void weave(tui_surface *sf, tui_rect a)
{
    const uint8_t at = ink(bay() ? (TUI_BLACK | TUI_BRIGHT) : TUI_BLUE);
    for (int y = a.y; y < a.y + a.h; y++)
        for (int x = a.x; x < a.x + a.w; x++)
            ls_box_put(sf, a, x, y, ((x + y) & 1) ? LS_BOX_DIAG_L : LS_BOX_DIAG_R, at);
}

/* A printed circuit: traces that turn on a 12x6 tile, pads where they end. */
static void circuit(tui_surface *sf, tui_rect a)
{
    /* No crossings: runs that turn once or twice and end on a pad. */
    static const char *const T[6] = {
        "──┐  •──┐   ",
        "  │     └── ",
        "  └──•      ",
        "•──┐   ┌──• ",
        "   │   │    ",
        "   └───┘    ",
    };
    const uint8_t trace = ink(bay() ? TUI_GREEN : TUI_BLUE), pad = ink(bay() ? TUI_YELLOW : (TUI_YELLOW | TUI_BRIGHT));
    for (int y = a.y; y < a.y + a.h; y++) {
        const char *row = T[(y - a.y) % 6];
        for (int x = a.x; x < a.x + a.w; ) {
            /* walk the UTF-8 row once per tile repeat */
            const char *p = row;
            for (int k = 0; k < 12 && x < a.x + a.w; k++, x++) {
                char one[5] = {0};
                int len = ((unsigned char)*p >= 0xE0) ? 3 : ((unsigned char)*p >= 0xC0 ? 2 : 1);
                memcpy(one, p, (size_t)len);
                p += len;
                if (one[0] == ' ') continue;
                dk_text(sf, a, x, y, one, !strcmp(one, "\xe2\x80\xa2") ? pad : trace);
            }
        }
    }
}

/* The mark: a fin cutting the water, in lines, over the name spaced out. */
static void logo(tui_surface *sf, tui_rect a)
{
    const int fh = 9;                                  /* the fin's height in rows */
    if (a.w < 40 || a.h < fh + 8) return;
    const int cx = a.x + a.w / 2, top = a.y + (a.h - fh - 6) / 2;
    /* a mid ground (teal, green, plum) takes white, where blue would sink */
    const uint8_t g = TUI_ATTR_BG(dk_attr(R_DESK));
    const bool mid = !bay() && (g == TUI_CYAN || g == TUI_GREEN || g == TUI_MAGENTA);
    const uint8_t fin = ink(bay() ? (TUI_CYAN | TUI_BRIGHT) : mid ? (TUI_WHITE | TUI_BRIGHT) : TUI_BLUE);
    const uint8_t back = ink(bay() ? (TUI_WHITE | TUI_BRIGHT) : mid ? TUI_BLACK : (TUI_WHITE | TUI_BRIGHT));
    /* leading edge: a slope two cells a row; trailing edge: straight down */
    for (int r = 0; r < fh; r++) {
        ls_box_put(sf, a, cx - 2 * r - 1, top + r, LS_BOX_DIAG_R, fin);
        ls_box_put(sf, a, cx - 2 * r - 2, top + r, LS_BOX_DIAG_R, fin);
        ls_box_put(sf, a, cx + 1 + (r > fh / 2 ? (r - fh / 2) / 2 : 0), top + r, LS_BOX_DIAG_L, back);
    }
    /* the waterline and the sea under it */
    const int wl = top + fh, half = 2 * fh + 6;
    const uint8_t sea1 = ink(bay() ? (TUI_BLUE | TUI_BRIGHT) : TUI_BLUE),
                  sea2 = ink(bay() ? TUI_BLUE : mid ? (TUI_WHITE | TUI_BRIGHT) : (TUI_CYAN | TUI_BRIGHT));
    for (int x = cx - half; x <= cx + half; x++) {
        tui_put_char(sf, a, x, wl, ((x - cx) & 3) < 2 ? '~' : '-', sea1);
        if (((x - cx + 2) & 7) < 3) tui_put_char(sf, a, x, wl + 1, '~', sea2);
    }
    /* the name, spaced, in two colours, and what it is */
    static const char NAME[] = "L A K E S H A R K";
    const int nl = (int)sizeof(NAME) - 1;
    for (int i = 0; i < nl; i++)
        if (NAME[i] != ' ')
            tui_put_char(sf, a, cx - nl / 2 + i, wl + 3, NAME[i], ink(i < 7 ? (bay() ? (TUI_CYAN | TUI_BRIGHT) : mid ? (TUI_WHITE | TUI_BRIGHT) : TUI_BLUE)
                                                                      : (bay() ? (TUI_WHITE | TUI_BRIGHT) : TUI_BLACK)));
    const char *sub = "D E S K T O P";
    dk_text(sf, a, cx - (int)strlen(sub) / 2, wl + 5, sub, ink(bay() ? TUI_WHITE : (TUI_WHITE | TUI_BRIGHT)));
}

/* The receiver's sky: rings at 0, 30 and 60 degrees of elevation, north up,
   each satellite where it is, coloured by its signal. Nothing moves until
   the receiver reports a new sky. */
static void sky(tui_surface *sf, tui_rect a)
{
    if (a.w < 30 || a.h < 12) return;
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    const int cx = a.x + a.w / 2, cy = a.y + a.h / 2;
    /* radius in rows; columns scaled by the cell's aspect */
    const int rr = (a.h - 4) / 2;
    const uint8_t ring = ink(bay() ? (TUI_BLACK | TUI_BRIGHT) : TUI_BLUE);
    for (int k = 1; k <= 3; k++) {
        const int r = rr * k / 3;
        for (int d = 0; d < 360; d += 2) {
            const double t = d * 3.14159265358979 / 180.0;
            const int x = cx + (int)(r * sinf((float)t) * ch / cw), y = cy - (int)(r * cosf((float)t));
            ls_braille_put(sf, a, x, y, (int16_t)(0x2800 | 0x24), ring);
        }
    }
    tui_put_char(sf, a, cx, cy - rr - 1, 'N', ink(bay() ? (TUI_WHITE | TUI_BRIGHT) : TUI_BLACK));
    if (!ls_gps_get) return;
    ls_gps_get(&s_sky);
    char line[48];
    if (!s_sky.alive || !s_sky.sat_count) {
        snprintf(line, sizeof(line), "%s", s_sky.alive ? "no satellites in view" : "no GPS receiver");
        dk_text(sf, a, cx - dk_text_len(line) / 2, cy + rr + 1, line, ink(bay() ? TUI_WHITE : TUI_BLACK));
        return;
    }
    for (int i = 0; i < s_sky.sat_count && i < LS_GPS_MAX_SATS; i++) {
        const ls_gps_sat_t *s = &s_sky.sats[i];
        if (!s->prn) continue;
        const int r = rr * (90 - (s->elevation > 90 ? 90 : s->elevation)) / 90;
        const double t = s->azimuth * 3.14159265358979 / 180.0;
        const int x = cx + (int)(r * sinf((float)t) * ch / cw), y = cy - (int)(r * cosf((float)t));
        const uint8_t col = s->snr >= 35 ? TUI_GREEN | TUI_BRIGHT : s->snr >= 20 ? TUI_YELLOW | TUI_BRIGHT
                          : s->snr > 0 ? TUI_RED | TUI_BRIGHT : (TUI_BLACK | TUI_BRIGHT);
        ls_box_put(sf, a, x, y, LS_SHAPE_DOT, ink(s->used ? col : (uint8_t)(col & 7)));
        snprintf(line, sizeof(line), "%d", s->prn);
        dk_text(sf, a, x + 1, y, line, ink(bay() ? TUI_WHITE : TUI_BLACK));
    }
    snprintf(line, sizeof(line), "%d used of %d in view", s_sky.sats_used, s_sky.sats_visible);
    dk_text(sf, a, cx - dk_text_len(line) / 2, cy + rr + 1, line, ink(bay() ? TUI_WHITE : TUI_BLACK));
}

/* ----------------------------------------------------------- pictures -- */

static const char *file_of(uint32_t id)
{
    if (s_nfiles < 0) s_nfiles = dk_sys_wallpaper_files(s_files, 8);
    for (int i = 0; i < s_nfiles; i++) if (name_id(s_files[i]) == id) return s_files[i];
    return NULL;
}

static bool picture(tui_surface *sf, tui_rect a)
{
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    const int fit = dk_prefs()->fit;
    if (!s_px || s_px_id != s_cur || s_area_w != a.w || s_area_h != a.h || s_px_fit != fit) {
        if (s_failed && s_area_w == a.w && s_area_h == a.h && s_px_fit == fit) return false;
        drop_picture();
        s_area_w = a.w;
        s_area_h = a.h;
        s_px_fit = fit;
        /* At most 960 pixels across: image cells scale it to the area. */
        int w = a.w * cw, h = a.h * ch;
        const int full_w = w;
        if (w > 960) { h = h * 960 / w; w = 960; }
        const char *name = file_of(s_cur);
        if (!name || !dk_sys_load_wallpaper(name, w, h, fit, full_w, TUI_ATTR_BG(ground()), &s_px, &s_pw, &s_ph)) {
            s_failed = true;
            return false;
        }
        s_failed = false;
        s_px_id = s_cur;
        s_serial++;
    }
    const uint8_t g = ground();
    for (int y = a.y; y < a.y + a.h; y++)
        for (int x = a.x; x < a.x + a.w; x++)
            tui_put_char(sf, a, x, y, LS_TUI_IMAGE_CELL, g);
    ls_tui_image(a, s_px, s_pw, s_ph, s_serial);
    return true;
}

int dk_wall_pattern_count(void) { return WP_CIRCUIT + 1; }
const char *dk_wall_pattern_name(int i) { return i == 0 ? "(None)" : dk_wall_builtin_name(i); }

static void pattern(tui_surface *sf, tui_rect a, int pat)
{
    switch (pat) {
    case WP_DOTS: dots(sf, a); break;
    case WP_WAVES: waves(sf, a); break;
    case WP_WEAVE: weave(sf, a); break;
    case WP_CIRCUIT: circuit(sf, a); break;
    default: break;
    }
}

/* The pattern, then the wallpaper over it: a picture, the mark or the sky. */
void dk_wall_draw(tui_surface *sf, tui_rect a)
{
    dk_fill(sf, a, ground());
    int pat = dk_prefs()->pattern;
    if (!pat && s_cur >= WP_DOTS && s_cur <= WP_CIRCUIT) pat = (int)s_cur;   /* chosen before patterns had their own list */
    if (s_cur & DK_WP_FILE) {
        if (!picture(sf, a)) pattern(sf, a, pat ? pat : WP_DOTS);  /* the card is out, or the file went */
        return;
    }
    pattern(sf, a, pat);
    if (s_cur == WP_LOGO) logo(sf, a);
    else if (s_cur == WP_SKY) sky(sf, a);
}

/* The nearest of the theme's sixteen to a pixel, for the small monitor. */
static uint8_t nearest(uint16_t px)
{
    const ls_tui_theme_t *t = ls_tui_theme_effective(ls_tui_get_theme(), ls_tui_daylight());
    const int r = px >> 11 & 31, g = px >> 5 & 63, b = px & 31;
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; i++) {
        const uint16_t q = t->palette[i];
        const int dr = (q >> 11 & 31) - r, dg = ((q >> 5 & 63) - g) / 2, db = (q & 31) - b;
        const int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}

/* What the desktop would look like with these choices, in a small rect: the
   pattern, the wallpaper (a picture by its nearest colours, two pixels a
   cell), and a sliver of taskbar along the bottom. */
void dk_wall_preview(tui_surface *sf, tui_rect r, const dk_prefs_t *p)
{
    if (r.w < 4 || r.h < 3) return;
    const tui_rect screen = tui_rect_make(r.x, r.y, r.w, r.h - 1);
    dk_fill(sf, screen, ground());
    pattern(sf, screen, p->pattern);
    const uint32_t w = p->wall;
    if (w & DK_WP_FILE) {
        const char *name = file_of(w);
        if (name && (!s_pv || s_pv_id != w)) {
            if (s_pv) dk_psram_free(s_pv);
            s_pv = NULL;
            if (dk_sys_load_wallpaper(name, screen.w, screen.h * 2, p->fit, screen.w * 8, TUI_ATTR_BG(ground()),
                                      &s_pv, &s_pvw, &s_pvh))
                s_pv_id = w;
        }
        if (s_pv && s_pv_id == w)
            for (int y = 0; y < screen.h; y++)
                for (int x = 0; x < screen.w; x++) {
                    const uint8_t top = nearest(s_pv[(size_t)(2 * y) * s_pvw + x]);
                    const uint8_t bot = nearest(s_pv[(size_t)(2 * y + 1) * s_pvw + x]);
                    tui_put_char(sf, screen, screen.x + x, screen.y + y, LS_TUI_BLOCK_UPPER, TUI_ATTR(top, bot));
                }
    } else if (w == WP_LOGO) {
        const int cx = screen.x + screen.w / 2, cy = screen.y + screen.h / 2;
        const uint8_t fin = ink(bay() ? (TUI_CYAN | TUI_BRIGHT) : TUI_BLUE);
        ls_box_put(sf, screen, cx - 2, cy, LS_BOX_DIAG_R, fin);
        ls_box_put(sf, screen, cx - 1, cy - 1, LS_BOX_DIAG_R, fin);
        ls_box_put(sf, screen, cx, cy - 1, LS_BOX_DIAG_L, ink(bay() ? (TUI_WHITE | TUI_BRIGHT) : TUI_BLACK));
        for (int x = cx - 5; x <= cx + 5; x++) tui_put_char(sf, screen, x, cy + 1, '~', ink(TUI_BLUE | TUI_BRIGHT));
    } else if (w == WP_SKY) {
        const int cx = screen.x + screen.w / 2, cy = screen.y + screen.h / 2;
        for (int d = 0; d < 360; d += 30)
            ls_braille_put(sf, screen, cx + (int)(screen.h * 0.9f * sinf(d * 0.01745f)), cy - (int)(screen.h * 0.4f * cosf(d * 0.01745f)),
                           (int16_t)(0x2800 | 0x24), ink(bay() ? (TUI_BLACK | TUI_BRIGHT) : TUI_BLUE));
        tui_put_char(sf, screen, cx + 2, cy - 1, '*', ink(TUI_GREEN | TUI_BRIGHT));
        tui_put_char(sf, screen, cx - 3, cy + 1, '*', ink(TUI_YELLOW | TUI_BRIGHT));
    }
    /* the taskbar, with its Start key */
    const int ty = r.y + r.h - 1;
    dk_fill(sf, tui_rect_make(r.x, ty, r.w, 1), dk_attr(R_FACE));
    dk_fill(sf, tui_rect_make(r.x, ty, 3, 1), dk_attr(R_TASKKEY));
    ls_box_put(sf, r, r.x + 1, ty, LS_BOX_DIAG_R, (uint8_t)((dk_attr(R_TASKKEY) & 0xF0) | TUI_CYAN));
    dk_text(sf, r, r.x + r.w - 6, ty, "12:00", dk_attr(R_STRONG));
}

void dk_wall_after_windows(tui_surface *sf, tui_rect a, tui_rect keep)
{
    if (!(s_cur & DK_WP_FILE) || !s_px) return;
    const uint16_t *src = NULL;
    if (ls_tui_image_now(NULL, &src, NULL, NULL, NULL) && src == s_px) return;
    /* The front app took the one picture: its cells are its own, and the
       wallpaper's fall back to the plain ground until it lets go. */
    for (int y = a.y; y < a.y + a.h; y++)
        for (int x = a.x; x < a.x + a.w; x++) {
            if (tui_rect_contains(keep, x, y)) continue;
            tui_cell *c = &sf->back[(size_t)y * sf->w + x];
            if (c->ch < 0x100 && (uint8_t)c->ch == (uint8_t)LS_TUI_IMAGE_CELL) c->ch = ' ';
        }
}

/* Over a picture, what the desk draws on the ground (icons, their labels,
   the wordmark, the bar's top line) is glass: its glyph over the picture
   instead of on a block of the desk's colour. A window drawn later over a
   cell takes it back. */
void dk_wall_glass(tui_surface *sf, tui_rect a)
{
    if (!(s_cur & DK_WP_FILE) || !s_px) return;
    a = tui_rect_intersect(a, tui_surface_rect(sf));
    for (int y = a.y; y < a.y + a.h; y++)
        for (int x = a.x; x < a.x + a.w; x++) {
            const tui_cell *c = &sf->back[(size_t)y * sf->w + x];
            if (c->ch < 0x100 && (uint8_t)c->ch == (uint8_t)LS_TUI_IMAGE_CELL) continue;
            /* Only what is drawn on the bare ground: a cell whose own
               ground is a colour (two-colour icon art, a selected label)
               stays solid, or its art would lose half its colours. */
            if (TUI_ATTR_BG(c->attr) != TUI_ATTR_BG(ground())) continue;
            ls_tui_glass(x, y);
        }
}
