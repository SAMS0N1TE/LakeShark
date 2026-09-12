/* See ls_tui.h for why the TUI bypasses LVGL entirely. */
#include "ls_tui.h"
#include "ls_tui_inset.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ls_font.h"
#include "ls_theme.h"

#include "ls_panel.h"

static const char *TAG = "ls_tui";

/* The active face. Everything downstream reads its metrics rather
   than a constant, so changing size is one call and a re-begin. */
static const ls_font_t *s_font = &ls_font_mono_16;
static tui_surface  s_surface;
static tui_cell    *s_back, *s_front;
static int          s_cols, s_rows, s_cw, s_ch;
/* The panel's corners are physically rounded and were cutting the edge
   cells off. A terminal can simply start further in: the grid is inset by a
   whole number of cells on every side, so no cell can ever land under the
   arc.
   How far in is ls_tui_corner_inset's answer, not the radius. */
static int          s_ox, s_oy;
/* The corner radius in pixels. A physical fact of the glass, so whoever
   assembles the interface sets it from the board header; 40 is what this
   file assumed for the panel it was written on. */
static int          s_corner_r = 40;
/* The radius the grid on the glass was laid out against. 'tui corner'
   changes s_corner_r at once and the grid only at the regrid that follows,
   and the chrome's corner padding has to describe the grid it is drawn on. */
static int          s_grid_r = 40;
static int          s_screen_w, s_screen_h;      /* logical, landscape aware */
static bool         s_landscape, s_cw_rotation = true;
static uint32_t     s_last_us;
static int          s_last_cells;

static const ls_tui_theme_t *s_theme = &ls_theme_terminal_bay;
/* Daylight rides over s_theme instead of replacing it, so turning it
   off gives back whatever was chosen - including a theme chosen while it was
   on. s_draw is the palette the blitter actually indexes, and it changes only
   in look_apply at the top of a present: the console sets looks from its own
   task, and swapping the palette under a present half way through a frame
   would leave cells marked drawn in colours that are no longer the theme. */
static bool                  s_daylight;
static const ls_tui_theme_t *s_draw = &ls_theme_terminal_bay;
static volatile bool         s_look_dirty;
#define PALETTE (s_draw->palette)

static inline uint16_t attr_fg(uint8_t a) { return PALETTE[TUI_ATTR_FG(a) & 0x0F]; }
static inline uint16_t attr_bg(uint8_t a) { return PALETTE[TUI_ATTR_BG(a) & 0x0F]; }

/* Blend fg over bg in RGB565, no division.

   The obvious form divides each channel by 255 and that was three integer
   divides per anti-aliased pixel. RISC-V divide is multi-cycle and the font
   is 4 bpp, so most glyph pixels were paying it. Shifting by 256 instead is
   off by one part in 255 - under half a step of a 5-bit channel, invisible -
   and turns three divides into three shifts. */
static inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a)
{
    if (a == 0)   return bg;
    if (a >= 252) return fg;
    uint32_t ia = 255u - a;
    uint32_t r = ((((fg >> 11) & 0x1F) * a + (((bg >> 11) & 0x1F) * ia)) + 128) >> 8;
    uint32_t g = ((((fg >> 5) & 0x3F) * a + (((bg >> 5) & 0x3F) * ia)) + 128) >> 8;
    uint32_t b = (((fg & 0x1F) * a + ((bg & 0x1F) * ia)) + 128) >> 8;
    if (r > 0x1F) r = 0x1F;
    if (g > 0x3F) g = 0x3F;
    if (b > 0x1F) b = 0x1F;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Word-at-a-time run fill, falling back to halfwords at an odd start or a
   trailing pixel. The framebuffer base is aligned, so the odd case only comes
   up when a cell starts on an odd pixel. */
static inline void fill_run(uint16_t *run, int n, uint16_t bg, uint32_t bg2)
{
    int i = 0;
    if (((uintptr_t)run & 3u) && n > 0) { run[0] = bg; i = 1; }
    uint32_t *w = (uint32_t *)(void *)(run + i);
    for (; i + 1 < n; i += 2) *w++ = bg2;
    for (; i < n; i++) run[i] = bg;
}

static bool blit_block(uint16_t *fb, int native_w, int x0, int y0,
                       uint8_t ch, uint16_t fg, uint16_t bg)
{
    if (ch < 0x80 || ch > 0xA7) return false;

    if (ch >= 0xA0) {
        /* Thin trace, eighth-height. Solid blocks read as bars and a
           spectrum wants a line: two pixels wide, centred, rising from the
           bottom in eighths of a cell. That keeps the fine look of a drawn
           character while giving eight times the vertical precision a
           character can express, which is the whole reason to leave text
           behind for plots. */
        /* The cell's ground first, then the line. */

        {
            const uint32_t bg2 = ((uint32_t)bg << 16) | bg;
            if (s_landscape) {
                int n = s_ch;
                if (y0 + n > s_screen_h) n = s_screen_h - y0;
                for (int x = 0; x < s_cw && x0 + x < s_screen_w; x++)
                    fill_run(fb + (uint32_t)(s_screen_w - 1 - (x0 + x)) * native_w + y0,
                             n, bg, bg2);
            } else {
                int n = s_cw;
                if (x0 + n > s_screen_w) n = s_screen_w - x0;
                for (int y = 0; y < s_ch && y0 + y < s_screen_h; y++)
                    fill_run(fb + (uint32_t)(y0 + y) * native_w + x0, n, bg, bg2);
            }
        }
        int eighths = (ch - 0xA0) + 1;
        int h = s_ch * eighths / 8;
        if (h < 1) h = 1;
        int cx = x0 + s_cw / 2 - 1;
        for (int x = cx; x < cx + 2 && x < s_screen_w; x++) {
            if (x < 0) continue;
            for (int y = s_ch - h; y < s_ch; y++) {
                int py = y0 + y;
                if (py < 0 || py >= s_screen_h) continue;
                uint32_t idx = s_landscape
                    ? (uint32_t)(s_screen_w - 1 - x) * native_w + py
                    : (uint32_t)py * native_w + x;
                fb[idx] = fg;
            }
        }
        return true;
    }

    if (ch >= 0x90) {
        /* Shades: one flat colour, mixed. 0x93 is a solid fill. */
        static const uint8_t mix[4] = { 64, 128, 191, 255 };
        uint16_t c = blend565(fg, bg, mix[ch - 0x90]);
        for (int x = 0; x < s_cw && x0 + x < s_screen_w; x++) {
            int n = s_ch;
            if (y0 + n > s_screen_h) n = s_screen_h - y0;
            if (s_landscape) {
                uint16_t *run = fb + (uint32_t)(s_screen_w - 1 - (x0 + x)) * native_w + y0;
                fill_run(run, n, c, ((uint32_t)c << 16) | c);
            } else {
                for (int y = 0; y < n; y++)
                    fb[(uint32_t)(y0 + y) * native_w + x0 + x] = c;
            }
        }
        return true;
    }

    const int hw = s_cw / 2, hh = s_ch / 2;
    for (int qy = 0; qy < 2; qy++) {
        for (int qx = 0; qx < 2; qx++) {
            uint16_t c = (ch & (1u << (qy * 2 + qx))) ? fg : bg;
            int sx = x0 + qx * hw, ex = qx ? s_cw : hw;
            int sy = y0 + qy * hh, ey = qy ? s_ch : hh;
            for (int x = sx; x < x0 + ex && x < s_screen_w; x++) {
                for (int y = sy; y < y0 + ey && y < s_screen_h; y++) {
                    uint32_t idx = s_landscape
                        ? (uint32_t)(s_screen_w - 1 - x) * native_w + y
                        : (uint32_t)y * native_w + x;
                    fb[idx] = c;
                }
            }
        }
    }
    return true;
}

/* A coverage ramp instead of a blend per pixel. */

static uint16_t s_ramp[16];
static uint8_t  s_ramp_attr = 0xFF;
static bool     s_ramp_valid;

/* sRGB decode/encode over the 5- and 6-bit channels, small enough to inline
   and only run sixteen times per attribute. */
static inline uint32_t srgb_to_lin(uint32_t v, uint32_t max)
{
    uint32_t x = v * 255u / max;               /* 0..255 */
    return (x * x * 255u) / (255u * 255u) * 255u / 255u + (x * 30u) / 255u;
}

/* Audited for Daylight and left as it is. Both ends of the ramp come
   from the cell's own attribute - its foreground over its own background -
   so dark ink on a white ground blends toward white exactly as light ink on
   black blends toward black, and nothing here knows which way round the
   theme is. test_tui_render_dump draws a glyph in both polarities and holds
   every pixel of it to lie between the two colours of its cell. */
static void ramp_build(uint8_t attr)
{
    const uint16_t fg = attr_fg(attr), bg = attr_bg(attr);
    const uint32_t fr = (fg >> 11) & 0x1F, fgn = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    const uint32_t br = (bg >> 11) & 0x1F, bgn = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    for (uint32_t i = 0; i < 16; i++) {
        /* Perceptual weighting: square the coverage into light and back out
           again. Cheap approximation of decode-lerp-encode that keeps thin
           stems from disappearing on a dark ground. */
        uint32_t a = i * 17u;                      /* 0..255 */
        uint32_t lin = (a * a + 127u) / 255u;      /* a^2 in 0..255 */
        uint32_t w = (a + lin) / 2u;               /* halfway to linear */
        uint32_t iw = 255u - w;
        uint32_t r = (fr * w + br * iw + 128u) >> 8;
        uint32_t g = (fgn * w + bgn * iw + 128u) >> 8;
        uint32_t b = (fb * w + bb * iw + 128u) >> 8;
        if (r > 0x1F) r = 0x1F;
        if (g > 0x3F) g = 0x3F;
        if (b > 0x1F) b = 0x1F;
        s_ramp[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
    s_ramp_attr = attr;
    s_ramp_valid = true;
}

static inline void ramp_for(uint8_t attr)
{
    if (!s_ramp_valid || attr != s_ramp_attr) ramp_build(attr);
}

static void blit_cell(uint16_t *fb, int native_w, int native_h,
                      int col, int row, const tui_cell *cell)
{
    (void)native_h;
    const int x0 = s_ox + col * s_cw, y0 = s_oy + row * s_ch;
    const uint16_t fg = attr_fg(cell->attr), bg = attr_bg(cell->attr);

    /* Blocks paint the whole cell themselves, background included. */
    if (blit_block(fb, native_w, x0, y0, (uint8_t)cell->ch, fg, bg)) return;

    const ls_font_glyph_t *dsc = ls_font_glyph(s_font, (uint8_t)cell->ch);
    const uint8_t *bmp = dsc ? &s_font->bitmap[dsc->bitmap_index] : NULL;
    const bool have = dsc != NULL;
    if (have) ramp_for(cell->attr);

    /* Walk whichever axis is contiguous in the framebuffer. Portrait
       stores a row of pixels next to each other; landscape, because the grid
       is transposed onto a portrait panel, stores a *column* next to each
       other. Writing across the stride instead of along it was costing 12.9 us
       a cell for 170 pixels, which is most of a PSRAM burst thrown away per
       write. */
    /* Two pixels per store. The background is the bulk of the work -
       every cell fills all 170 px whether or not a glyph lands on them - and
       RGB565 means two fit in a word. Halves the store count on the run that
       dominates the blit. */
    const uint32_t bg2 = ((uint32_t)bg << 16) | bg;
    if (s_landscape) {
        int n = s_ch;
        if (y0 + n > s_screen_h) n = s_screen_h - y0;
        for (int x = 0; x < s_cw; x++) {
            int px = x0 + x;
            if (px >= s_screen_w) break;
            uint16_t *run = fb + (uint32_t)(s_screen_w - 1 - px) * native_w + y0;
            fill_run(run, n, bg, bg2);
        }
    } else {
        int n = s_cw;
        if (x0 + n > s_screen_w) n = s_screen_w - x0;
        for (int y = 0; y < s_ch; y++) {
            int py = y0 + y;
            if (py >= s_screen_h) break;
            fill_run(fb + (uint32_t)py * native_w + x0, n, bg, bg2);
        }
    }
    if (!have) return;

    const int gx = x0 + dsc->ofs_x;
    const int gy = y0 + (s_ch - s_font->base_line) - dsc->box_h - dsc->ofs_y;

    for (int y = 0; y < dsc->box_h; y++) {
        int py = gy + y;
        if (py < 0 || py >= s_screen_h) continue;
        for (int x = 0; x < dsc->box_w; x++) {
            int px = gx + x;
            if (px < 0 || px >= s_screen_w) continue;
            uint32_t bit = (uint32_t)y * dsc->box_w + x;
            /* 4 bpp, high nibble first, exactly as lv_font_conv emits it. */
            uint8_t cov = (bit & 1) ? (bmp[bit >> 1] & 0x0F) : (bmp[bit >> 1] >> 4);
            if (!cov) continue;
            uint32_t idx = s_landscape
                ? (uint32_t)(s_screen_w - 1 - px) * native_w + py
                : (uint32_t)py * native_w + px;
            fb[idx] = s_ramp[cov];
        }
    }
}

/* The look, applied where it is safe to apply it: at the top of a
   present or in begin, on the task that owns the framebuffer. The palette
   the blitter indexes, the ramp built from it and the front grid all change
   together here, so no cell can be marked drawn in colours that have gone. */
static void look_apply(void)
{
    s_look_dirty = false;
    s_draw = ls_tui_theme_effective(s_theme, s_daylight);
    s_ramp_valid = false;
    ls_tui_invalidate();
}

/* Everything on the glass that is not a cell. */

static void fill_logical(uint16_t *fb, int native_w, int x, int y, int w,
                         int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_screen_w) w = s_screen_w - x;
    if (y + h > s_screen_h) h = s_screen_h - y;
    if (w <= 0 || h <= 0) return;
    const uint32_t c2 = ((uint32_t)c << 16) | c;
    if (s_landscape) {
        for (int px = x; px < x + w; px++)
            fill_run(fb + (uint32_t)(s_screen_w - 1 - px) * native_w + y,
                     h, c, c2);
    } else {
        for (int py = y; py < y + h; py++)
            fill_run(fb + (uint32_t)py * native_w + x, w, c, c2);
    }
}

static void paint_margin(uint16_t *fb, int native_w)
{
    const uint16_t g = PALETTE[0];
    const int gx1 = s_ox + s_cols * s_cw, gy1 = s_oy + s_rows * s_ch;
    fill_logical(fb, native_w, 0, 0, s_screen_w, s_oy, g);               /* top    */
    fill_logical(fb, native_w, 0, gy1, s_screen_w, s_screen_h - gy1, g); /* bottom */
    fill_logical(fb, native_w, 0, s_oy, s_ox, gy1 - s_oy, g);            /* left   */
    fill_logical(fb, native_w, gx1, s_oy, s_screen_w - gx1, gy1 - s_oy, g);
}

bool ls_tui_begin(int screen_w, int screen_h)
{
    ls_panel_fb_t fb;
    if (!ls_panel_fb(&fb)) {
        ESP_LOGE(TAG, "no panel framebuffer");
        return false;
    }
    /* Logical size is the caller's business: it knows the rotation, and the
       point of this file is that it no longer asks LVGL anything. */
    s_screen_w = screen_w > 0 ? screen_w : fb.width;
    s_screen_h = screen_h > 0 ? screen_h : fb.height;
    s_landscape = s_screen_w > s_screen_h;
    /* The chosen font becomes THE font here and nowhere else.

       This is the one moment the grid dimensions are settled, so it is the
       one moment the choice can take effect. Setting the index at any other
       time changes what the next session will build and nothing about this
       one, which is why changing it asks for a restart. */
    ls_tui_set_font(ls_tui_font_at(ls_tui_font_index()));
    s_cw = s_font->cell_w;
    s_ch = s_font->cell_h;

    /* "You made the entire gui too small for the actual screen, its pretty far away from the actual borders." */

    ls_tui_corner_inset(s_screen_w, s_screen_h, s_cw, s_ch,
                        s_corner_r, &s_ox, &s_oy);
    s_grid_r = s_corner_r;
    s_cols = (s_screen_w - 2 * s_ox) / s_cw;
    s_rows = (s_screen_h - 2 * s_oy) / s_ch;
    if (s_cols < 8 || s_rows < 4) return false;

    ls_tui_end();
    size_t cells = (size_t)s_cols * s_rows;
    s_back  = heap_caps_calloc(cells, sizeof(tui_cell), MALLOC_CAP_SPIRAM);
    s_front = heap_caps_calloc(cells, sizeof(tui_cell), MALLOC_CAP_SPIRAM);
    if (!s_back || !s_front) { ls_tui_end(); return false; }

    /* Wipe the whole panel, not just the grid. The TUI paints cells and
       nothing else, so whatever LVGL left behind - the status bar, and the
       margin outside the inset - stayed on the glass underneath. One memset of
       the framebuffer is cheaper than teaching every path to own its border. */
    /* With the ground of the palette about to be drawn, which is no
       longer always zero. The look is settled first, so the fill, the ramp
       and the first present agree; a present that changes it afterwards
       repaints the margin itself, in paint_margin. */
    look_apply();
    {
        const uint16_t g = PALETTE[0];
        const size_t n = (size_t)fb.width * fb.height;
        if (g == 0) memset(fb.pixels, 0, n * sizeof(uint16_t));
        else        fill_run(fb.pixels, (int)n, g, ((uint32_t)g << 16) | g);
    }
    ls_panel_fb_present();

    tui_surface_setup(&s_surface, s_back, s_front, s_cols, s_rows);
    ESP_LOGI(TAG, "grid %dx%d cells, %s at %dx%d px, inset %d,%d, %s",
             s_cols, s_rows, s_font->name, s_cw, s_ch, s_ox, s_oy,
             s_landscape ? "landscape" : "portrait");
    return true;
}

void ls_tui_end(void)
{
    free(s_back);  s_back = NULL;
    free(s_front); s_front = NULL;
    memset(&s_surface, 0, sizeof(s_surface));
}

tui_surface *ls_tui_surface(void) { return s_back ? &s_surface : NULL; }

void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch)
{
    if (cols) *cols = s_cols;
    if (rows) *rows = s_rows;
    if (cw)   *cw = s_cw;
    if (ch)   *ch = s_ch;
}

void ls_tui_invalidate(void)
{
    /* A cell that cannot occur forces every comparison to differ. */
    if (s_front) memset(s_front, 0xFF, (size_t)s_cols * s_rows * sizeof(tui_cell));
}

/* The blitter writes one transform and only one: every framebuffer index it computes is the clockwise `(s_screen_w - 1 - x) * native_w + y` form, inline in the run loops. */

/* Takes effect on the next begin(), like the font, and for the same
   reason: the inset settles the grid dimensions and the buffers are sized
   off those. Exposed so the number can be swept against the actual glass
   instead of inherited - a regrid re-runs begin() without a reflash. */
void ls_tui_set_corner_radius(int px)
{
    s_corner_r = px < 0 ? 0 : px;
}

int ls_tui_corner_radius(void) { return s_corner_r; }

/* See ls_tui.h. The arithmetic is ls_tui_inset.c's; this file only
   supplies the pixels, which nothing above it is allowed to hold. */
int ls_tui_corner_pad(int row)
{
    if (!s_back) return 0;
    return ls_tui_corner_cells(s_screen_w, s_screen_h, s_cw, s_ch,
                               s_grid_r, s_ox, s_oy, row);
}

void ls_tui_set_rotation_cw(bool clockwise)
{
    if (!clockwise) {
        ESP_LOGW(TAG, "counter-clockwise is not implemented in the blitter; "
                      "staying clockwise so touch matches the picture");
        return;
    }
    s_cw_rotation = true;
}

/* Takes effect on the next begin(): the grid dimensions come from the
   cell size, so the buffers have to be reallocated to change it. */
/* Which font is current. The list of them, and which one is wanted,
   is in ls_font_list.c: this file owns the framebuffer and cannot come to
   the host bench, and the SETTINGS screen that offers the choice can. */
void ls_tui_set_font(const ls_font_t *font) { if (font) s_font = font; }

/* The ramp cache is keyed on the attribute byte, which does not change
   when the colours behind it do, so a theme swap has to drop it explicitly or
   the first cells drawn afterwards keep the old colours. */
/* Now recorded here and carried out by look_apply, on the task that
   presents, which drops the ramp and repaints the grid and the margin in one
   place. See s_draw. */
void ls_tui_set_theme(const ls_tui_theme_t *theme)
{
    if (!theme) return;
    if (theme == &ls_theme_daylight) {      /* a toggle, never the choice */
        ls_tui_set_daylight(true);
        return;
    }
    s_theme = theme;
    s_look_dirty = true;
}
const ls_tui_theme_t *ls_tui_get_theme(void) { return s_theme; }

void ls_tui_set_daylight(bool on)
{
    s_daylight = on;
    s_look_dirty = true;
}

bool ls_tui_daylight(void) { return s_daylight; }

const ls_tui_theme_t *ls_tui_active_theme(void)
{
    return ls_tui_theme_effective(s_theme, s_daylight);
}
const ls_font_t *ls_tui_font(void) { return s_font; }

/* The rectangle a screen has borrowed. See ls_tui.h. */
static tui_rect s_reserved;

static inline bool reserved_cell(int col, int row)
{
    return s_reserved.w > 0 && s_reserved.h > 0 &&
           col >= s_reserved.x && col < s_reserved.x + s_reserved.w &&
           row >= s_reserved.y && row < s_reserved.y + s_reserved.h;
}

tui_rect ls_tui_reserved(void) { return s_reserved; }

void ls_tui_reserve(tui_rect cells)
{
    if (cells.w < 0) cells.w = 0;
    if (cells.h < 0) cells.h = 0;

    /* Handing a rectangle back has to repaint it. The front buffer still
       holds whatever the grid last had there, and the panel holds a map, so
       without this the two disagree and the diff renderer - which only pushes
       changed cells - would leave the map on screen under the next screen's
       chrome until something happened to change each cell. */
    if (s_front && s_reserved.w > 0 && s_reserved.h > 0) {
        for (int row = s_reserved.y; row < s_reserved.y + s_reserved.h; row++) {
            if (row < 0 || row >= s_rows) continue;
            for (int col = s_reserved.x; col < s_reserved.x + s_reserved.w; col++) {
                if (col < 0 || col >= s_cols) continue;
                /* An attribute no theme produces, so the cell differs from
                   whatever the back buffer holds and is redrawn once. */
                s_front[(size_t)row * s_cols + col].ch = 0;
                s_front[(size_t)row * s_cols + col].attr = 0xFF;
            }
        }
    }
    s_reserved = cells;
}

void ls_tui_blit_rgb565(tui_rect cells, const uint16_t *src,
                        int src_w, int src_h)
{
    if (!src || src_w <= 0 || src_h <= 0) return;
    if (cells.w <= 0 || cells.h <= 0) return;

    if (cells.x != s_reserved.x || cells.y != s_reserved.y ||
        cells.w != s_reserved.w || cells.h != s_reserved.h) return;

    ls_panel_fb_t fb;
    if (!ls_panel_fb(&fb)) return;

    const int x0 = s_ox + cells.x * s_cw;
    const int y0 = s_oy + cells.y * s_ch;
    int w = cells.w * s_cw;
    int h = cells.h * s_ch;
    if (w > src_w) w = src_w;
    if (h > src_h) h = src_h;
    if (x0 + w > s_screen_w) w = s_screen_w - x0;
    if (y0 + h > s_screen_h) h = s_screen_h - y0;
    if (w <= 0 || h <= 0) return;

    /* The same reasoning as the cell blitter: walk whichever axis is
       contiguous in the panel. Portrait stores a logical row next to itself;
       landscape stores a logical column next to itself, because the grid is
       transposed onto a portrait panel. */
    if (s_landscape) {
        for (int x = 0; x < w; x++) {
            const int px = x0 + x;
            uint16_t *run = fb.pixels +
                            (uint32_t)(s_screen_w - 1 - px) * fb.width + y0;
            const uint16_t *col = src + x;
            for (int y = 0; y < h; y++) run[y] = col[(size_t)y * src_w];
        }
    } else {
        for (int y = 0; y < h; y++) {
            uint16_t *run = fb.pixels + (uint32_t)(y0 + y) * fb.width + x0;
            memcpy(run, src + (size_t)y * src_w, (size_t)w * sizeof(uint16_t));
        }
    }
    ls_panel_fb_present();
}

int ls_tui_present(void)
{
    if (!s_back || !s_front) return 0;
    ls_panel_fb_t fb;
    if (!ls_panel_fb(&fb)) return 0;

    uint64_t t0 = esp_timer_get_time();
    /* A theme or Daylight change lands here, where the palette, the
       ramp, every cell and the margin outside the grid change together. */
    bool margin = false;
    if (s_look_dirty) {
        look_apply();
        paint_margin(fb.pixels, fb.width);
        margin = true;
    }
    int drawn = 0;
    for (int row = 0; row < s_rows; row++) {
        for (int col = 0; col < s_cols; col++) {
            if (reserved_cell(col, row)) continue;
            size_t i = (size_t)row * s_cols + col;
            if (s_back[i].ch == s_front[i].ch &&
                s_back[i].attr == s_front[i].attr) continue;
            blit_cell(fb.pixels, fb.width, fb.height, col, row, &s_back[i]);
            s_front[i] = s_back[i];
            drawn++;
        }
    }
    if (drawn || margin) ls_panel_fb_present();
    s_last_us = (uint32_t)(esp_timer_get_time() - t0);
    s_last_cells = drawn;
    return drawn;
}

/* Native pixel to grid cell. */

bool ls_tui_pixel_to_cell(int native_x, int native_y, int *col, int *row)
{
    if (!s_back) return false;
    int lx, ly;
    if (!s_landscape) {
        lx = native_x;
        ly = native_y;
    } else if (s_cw_rotation) {
        lx = s_screen_w - 1 - native_y;
        ly = native_x;
    } else {
        lx = native_y;
        ly = s_screen_h - 1 - native_x;
    }
    lx -= s_ox;
    ly -= s_oy;
    if (lx < 0 || ly < 0) return false;
    int c = lx / s_cw, r = ly / s_ch;
    if (c >= s_cols || r >= s_rows) return false;
    if (col) *col = c;
    if (row) *row = r;
    return true;
}

/* A printable stand-in for the procedural glyphs, so a dump of a waterfall
   still looks like one. The ramp runs light to dark the way the shades do. */
static char printable(char ch)
{
    unsigned char c = (unsigned char)ch;
    if (c >= 0x20 && c < 0x7F) return ch;
    if (c >= 0x80 && c <= 0x8F) return '#';        /* quadrant blocks */
    if (c == 0x90) return '.';                     /* 25% shade       */
    if (c == 0x91) return ':';                     /* 50%             */
    if (c == 0x92) return '+';                     /* 75%             */
    if (c == 0x93) return '#';                     /* full            */
    if (c >= 0xA0 && c <= 0xA7) return "_.,-=+*|"[c - 0xA0];  /* traces */
    return '?';
}

void ls_tui_dump(void)
{
    if (!s_back) { printf("tui: no grid\n"); return; }

    printf("tui: %dx%d cells, %s, %s\n", s_cols, s_rows,
           s_font ? s_font->name : "?",
           s_landscape ? "landscape" : "portrait");

    /* A column ruler every ten, so a cell named by the touch diagnostics can
       be found here without counting across. */
    printf("     ");
    for (int x = 0; x < s_cols; x++)
        putchar(x % 10 ? ' ' : (char)('0' + (x / 10) % 10));
    putchar('\n');

    for (int y = 0; y < s_rows; y++) {
        printf("%3d  ", y);
        for (int x = 0; x < s_cols; x++)
            putchar(printable(s_back[(size_t)y * s_cols + x].ch));
        putchar('\n');
    }
}

void ls_tui_last_cost(uint32_t *us, int *cells)
{
    if (us)    *us = s_last_us;
    if (cells) *cells = s_last_cells;
}
