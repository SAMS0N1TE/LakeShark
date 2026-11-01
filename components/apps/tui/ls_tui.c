#include "ls_braille.h"
#include "ls_box.h"
/* See ls_tui.h for why the TUI bypasses LVGL entirely. */
#include "ls_tui.h"
#include "ls_tui_inset.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ls_font.h"
#include "ls_theme.h"

#include "ls_panel.h"
#include "ls_cold.h"
#include "ls_textpack.h"

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
/* The camera hole in native portrait pixels; zero radius is none. */
static int          s_cutout_cx, s_cutout_cy, s_cutout_r;
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
static void tell_orientation(void);
/* Logical pixels to native portrait storage. */
static inline uint32_t landscape_index(int x, int y, int native_w)
{
    return s_cw_rotation ? (uint32_t)(s_screen_w - 1 - x) * native_w + y
                         : (uint32_t)x * native_w + s_screen_h - 1 - y;
}

/* The lowest address of a constant-colour logical column. */
static inline uint32_t landscape_run(int x, int y, int n, int native_w)
{
    return landscape_index(x, s_cw_rotation ? y : y + n - 1, native_w);
}

static uint32_t     s_last_us, s_max_us;
static int          s_last_cells;

static const ls_tui_theme_t *s_theme = &ls_theme_terminal_bay;
/* Daylight rides over s_theme instead of replacing it, so turning it
   off gives back whatever was chosen - including a theme chosen while it was
   on. s_draw is the palette the blitter actually indexes, and it changes only
   in look_apply at the top of a present: the console sets looks from its own
   task, and swapping the palette under a present half way through a frame
   would leave cells marked drawn in colours that are no longer the theme. */
static bool                  s_daylight;
static bool                  s_crisp_text;
static bool                  s_draw_crisp;
static const ls_tui_theme_t *s_draw = &ls_theme_terminal_bay;
static volatile bool         s_look_dirty;
#define PALETTE (s_draw->palette)
/* MAP snapshots only the basemap cells. Overlay replacements use the ordinary
 * theme. All per-cell storage and both ramp banks live in PSRAM. */
static uint16_t s_base_palette[16];
static const uint16_t *s_cell_palette;
/* A glass overlay over the basemap (a range ring, a route): the theme's ink
   on the basemap's ground, so the map's colour shows behind it, not the
   theme's black. */
static const uint16_t *s_cell_bg_palette;
static tui_rect s_base_rect;
static tui_cell *s_base_rec;
static uint8_t *s_base_front;
static bool s_base_dirty;
#define CELL_PALETTE (s_cell_palette ? s_cell_palette : PALETTE)
/* Ramp bank: 0 theme, 1 basemap, 2 theme ink on basemap ground. */
#define RAMP_BANK (s_cell_palette ? 1 : s_cell_bg_palette ? 2 : 0)

static inline uint16_t attr_fg(uint8_t a) { return CELL_PALETTE[TUI_ATTR_FG(a) & 0x0F]; }
static inline uint16_t attr_bg(uint8_t a)
{
    return (s_cell_bg_palette ? s_cell_bg_palette : CELL_PALETTE)[TUI_ATTR_BG(a) & 0x0F];
}

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
                       uint16_t ch, uint16_t fg, uint16_t bg)
{
    if(ch>=0xff00) ch=(uint8_t)ch;
    if(ch>=0x2800 && ch<=0x28ff) {
        for(int y=0;y<s_ch && y0+y<s_screen_h;y++)
            for(int x=0;x<s_cw && x0+x<s_screen_w;x++) {
                uint32_t idx=s_landscape ? landscape_index(x0+x,y0+y,native_w)
                                         : (uint32_t)(y0+y)*native_w+x0+x;
                fb[idx]=ls_braille_pixel(ch,x,y,s_cw,s_ch)?fg:bg;
            }
        return true;
    }
    if (ch >= 0x2000 && ls_box_is(ch)) {
        ls_box_mask_t m;
        ls_box_mask(ch, s_cw, s_ch, &m);
        for (int y = 0; y < s_ch && y0 + y < s_screen_h; y++) {
            const uint32_t bits = y < 32 ? m.r[y] : 0;
            for (int x = 0; x < s_cw && x0 + x < s_screen_w; x++) {
                const uint32_t idx = s_landscape ? landscape_index(x0 + x, y0 + y, native_w)
                                                 : (uint32_t)(y0 + y) * native_w + x0 + x;
                fb[idx] = (x < 32 && (bits >> x & 1)) ? fg : bg;
            }
        }
        return true;
    }
    if (ch < 0x80 || (ch > 0xA7 && ch < 0xC0)) return false;

    if (ch >= 0xC0) {
        /* Sextant: two columns, three rows, each ink or paper. */
        const int hw = s_cw / 2;
        for (int sy = 0; sy < 3; sy++) {
            const int ya = y0 + s_ch * sy / 3, yb = y0 + s_ch * (sy + 1) / 3;
            for (int sx = 0; sx < 2; sx++) {
                const uint16_t c = (ch & (1u << (sy * 2 + sx))) ? fg : bg;
                const int xa = x0 + (sx ? hw : 0), xb = x0 + (sx ? s_cw : hw);
                for (int x = xa; x < xb && x < s_screen_w; x++)
                    for (int y = ya; y < yb && y < s_screen_h; y++) {
                        const uint32_t idx = s_landscape
                            ? landscape_index(x,y,native_w)
                            : (uint32_t)y * native_w + x;
                        fb[idx] = c;
                    }
            }
        }
        return true;
    }

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
                    fill_run(fb + landscape_run(x0+x,y0,n,native_w),
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
                    ? landscape_index(x,py,native_w)
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
        const uint32_t c2 = ((uint32_t)c << 16) | c;
        if (s_landscape) {
            int n = s_ch;
            if (y0 + n > s_screen_h) n = s_screen_h - y0;
            for (int x = 0; x < s_cw && x0 + x < s_screen_w; x++)
                fill_run(fb + landscape_run(x0+x,y0,n,native_w),
                         n, c, c2);
        } else {
            int n = s_cw;
            if (x0 + n > s_screen_w) n = s_screen_w - x0;
            for (int y = 0; y < s_ch && y0 + y < s_screen_h; y++)
                fill_run(fb + (uint32_t)(y0 + y) * native_w + x0, n, c, c2);
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
                        ? landscape_index(x,y,native_w)
                        : (uint32_t)y * native_w + x;
                    fb[idx] = c;
                }
            }
        }
    }
    return true;
}

/* A coverage ramp instead of a blend per pixel. */

/* Alternating attributes used to rebuild the same sixteen colours hundreds
   of times on a screen switch. The table and validity bits live in PSRAM. */
static EXT_RAM_BSS_ATTR uint16_t s_ramps[3][256][16];
static EXT_RAM_BSS_ATTR bool s_ramp_valid[3][256];
static const uint16_t *s_ramp;

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
        s_ramps[RAMP_BANK][attr][i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
    s_ramp_valid[RAMP_BANK][attr] = true;
}

static uint32_t s_prof_ramps;

static inline void ramp_for(uint8_t attr)
{
    if (!s_ramp_valid[RAMP_BANK][attr]) { ramp_build(attr); s_prof_ramps++; }
    s_ramp = s_ramps[RAMP_BANK][attr];
}

static tui_rect s_image_rect;
static const uint16_t *s_image;
static int s_image_w, s_image_h;
static uint32_t s_image_serial;
static bool s_image_dirty;
static uint16_t *s_image_previous;
static bool s_image_previous_valid;
/* A scrolling picture kept as a ring (ls_tui_image_ring): column x0 / row y0
   of the source is the picture's left / top edge, and dx / dy is how far it
   has moved left / down since the last present. What was on the glass when
   that present finished, so the next one can tell whether moving those
   pixels is enough. */
static bool s_image_ring,s_image_pan;
static uint32_t s_pan_epoch;
static int32_t s_pan_left,s_pan_top;
static uint16_t s_pan_ground;
static uint32_t s_ring_epoch,s_ring_columns,s_ring_rows;
static int s_image_x0, s_image_y0, s_image_dx, s_image_dy;
static bool s_image_shown;
static tui_rect s_image_shown_rect;
static const uint16_t *s_image_shown_src;
static int s_image_shown_w, s_image_shown_h;
/* This present moved the pixels: only the cells the new lines land in draw. */
static bool s_image_shifted;
static uint32_t s_image_copy_bytes, s_present_flush_bytes;

void ls_tui_last_traffic(uint32_t *image_bytes, uint32_t *flush_bytes)
{
    if (image_bytes) *image_bytes = s_image_copy_bytes;
    if (flush_bytes) *flush_bytes = s_present_flush_bytes;
}

/* Glass: cells whose glyph is drawn over the image instead of on a solid
   ground. The mark records the cell as it was written, and a present honours
   it only while the back grid still holds exactly that, so anything drawn
   over the map afterwards (a picker, the keyboard) is solid again without
   having to know the map was there. */
static uint8_t  *s_glass_back, *s_glass_front;   /* one bit per cell */

/* Where the surface being drawn sits in the grid (ls_tui_set_origin): the
   desk draws an app into a window's own surface, and the rects that app
   hands over for pixels it owns are in that surface's cells. */
static int s_org_x, s_org_y;
static const tui_surface *s_org_sf;

void ls_tui_set_origin(const tui_surface *sf, int dx, int dy)
{
    s_org_sf = sf;
    s_org_x = sf ? dx : 0;
    s_org_y = sf ? dy : 0;
}

void ls_tui_get_origin(int *dx, int *dy)
{
    if (dx) *dx = s_org_x;
    if (dy) *dy = s_org_y;
}

static inline tui_rect org_rect(tui_rect r)
{
    r.x += s_org_x;
    r.y += s_org_y;
    return r;
}
static tui_cell *s_glass_rec;

static inline bool glass_bit(const uint8_t *bits, size_t i)
{
    return bits && (bits[i >> 3] >> (i & 7)) & 1;
}

static inline void glass_set(uint8_t *bits, size_t i, bool on)
{
    if (!bits) return;
    if (on) bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    else    bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

void ls_tui_basemap(tui_surface *sf,tui_rect area,const uint16_t palette[16])
{
    if(!palette || !sf || !s_base_rec) {
        if(s_base_rect.w) s_base_dirty=true;
        s_base_rect=tui_rect_make(0,0,0,0);return;
    }
    area=org_rect(area);
    if(memcmp(s_base_palette,palette,sizeof(s_base_palette)) ||
       memcmp(&s_base_rect,&area,sizeof(area))) {
        memcpy(s_base_palette,palette,sizeof(s_base_palette));
        memset(s_ramp_valid[1],0,sizeof(s_ramp_valid[1]));
        memset(s_ramp_valid[2],0,sizeof(s_ramp_valid[2]));s_base_dirty=true;
    }
    s_base_rect=area;
    for(int y=area.y;y<area.y+area.h;y++) for(int x=area.x;x<area.x+area.w;x++) {
        const int lx=x-s_org_x, ly=y-s_org_y;
        if(x>=0 && y>=0 && x<s_cols && y<s_rows && lx>=0 && ly>=0 && lx<sf->w && ly<sf->h)
            s_base_rec[y*s_cols+x]=sf->back[ly*sf->w+lx];
    }
}
static bool basemap_live(int col,int row,size_t i)
{
    return s_base_rec && tui_rect_contains(s_base_rect,col,row) &&
           s_back[i].ch==s_base_rec[i].ch && s_back[i].attr==s_base_rec[i].attr;
}

static void image_set(tui_rect cells, const uint16_t *src, int w, int h,
                      uint32_t serial, bool ring);
void ls_tui_image(tui_rect cells, const uint16_t *src, int w, int h, uint32_t serial)
{
    s_image_pan=false;
    cells = org_rect(cells);
    image_set(cells, src, w, h, serial, false);
    s_image_x0 = s_image_y0 = s_image_dx = s_image_dy = 0;
}

void ls_tui_image_pan(tui_rect cells,const uint16_t *src,int w,int h,uint32_t serial,
                      uint32_t epoch,int32_t left,int32_t top,uint16_t ground) {
    ls_tui_image(cells,src,w,h,serial);
    s_image_pan=true;s_pan_epoch=epoch;s_pan_left=left;s_pan_top=top;s_pan_ground=ground;
}
bool ls_tui_image_pan_state(uint32_t *epoch,int32_t *left,int32_t *top,uint16_t *ground) {
    *epoch=s_pan_epoch;*left=s_pan_left;*top=s_pan_top;*ground=s_pan_ground;
    return s_image_pan && s_image;
}
void ls_tui_image_ring(tui_rect cells, const uint16_t *src, int w, int h,
                       int x0, int y0, int dx, int dy, uint32_t serial)
{
    cells = org_rect(cells);
    s_image_pan=false;
    const bool same = s_image_ring && src == s_image && w == s_image_w && h == s_image_h &&
                      !memcmp(&cells, &s_image_rect, sizeof(cells));
    image_set(cells, src, w, h, serial, true);
    /* A negative move says the whole picture changed: draw every cell. */
    if (!same || dx < 0 || dy < 0) {
        s_image_dx = s_image_dy = 0;s_image_shown = false;
        s_ring_epoch++;s_ring_columns=s_ring_rows=0;
    }
    /* Moves since the last present add up; a present uses them once. */
    s_image_dx += dx > 0 ? dx : 0;
    s_image_dy += dy > 0 ? dy : 0;
    s_ring_columns += dx > 0 ? dx : 0;s_ring_rows += dy > 0 ? dy : 0;
    s_image_x0 = w > 0 ? ((x0 % w) + w) % w : 0;
    s_image_y0 = h > 0 ? ((y0 % h) + h) % h : 0;
}

bool ls_tui_has_images(void) { return s_back != NULL; }

void ls_tui_image_ring_offset(int *x0, int *y0)
{
    if (x0) *x0 = s_image_ring ? s_image_x0 : 0;
    if (y0) *y0 = s_image_ring ? s_image_y0 : 0;
}

bool ls_tui_image_ring_state(uint32_t *epoch,uint32_t *columns,uint32_t *rows)
{
    if(epoch) *epoch=s_ring_epoch;
    if(columns) *columns=s_ring_columns;
    if(rows) *rows=s_ring_rows;
    return s_image_ring && s_image && s_image_w>0 && s_image_h>0;
}

static void image_set(tui_rect cells, const uint16_t *src, int w, int h,
                      uint32_t serial, bool ring)
{
    if (s_glass_back) memset(s_glass_back, 0, ((size_t)s_cols * s_rows + 7) / 8);
    if (ring) {
        /* A ring is never compared against a copy of itself: the moves say
           what changed, and the copy would cost a full image every frame. */
        heap_caps_free(s_image_previous);
        s_image_previous = NULL;
        s_image_previous_valid = false;
    } else if (!src || s_image_ring || s_image_w != w || s_image_h != h ||
        memcmp(&s_image_rect, &cells, sizeof(cells))) {
        heap_caps_free(s_image_previous);
        s_image_previous = NULL;
        s_image_previous_valid = false;
        if (src && w > 0 && h > 0 && (size_t)w * h <= 256u * 1024u)
            s_image_previous = heap_caps_malloc((size_t)w * h * sizeof(uint16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_image != src || s_image_w != w || s_image_h != h || s_image_serial != serial ||
        memcmp(&s_image_rect, &cells, sizeof(cells)) || s_image_ring != ring) s_image_dirty = true;
    s_image_rect = cells;
    s_image = src;
    s_image_w = w; s_image_h = h;
    s_image_serial = serial;
    s_image_ring = ring;
}

bool ls_tui_image_now(tui_rect *cells, const uint16_t **src, int *w, int *h, uint32_t *serial)
{
    if (!s_image || s_image_w <= 0 || s_image_h <= 0) return false;
    if (cells)  *cells = s_image_rect;
    if (src)    *src = s_image;
    if (w)      *w = s_image_w;
    if (h)      *h = s_image_h;
    if (serial) *serial = s_image_serial;
    return true;
}

const uint8_t *ls_tui_glass_now(size_t *bytes)
{
    if (bytes) *bytes = ((size_t)s_cols * s_rows + 7) / 8;
    return s_glass_front;
}

static bool image_cell(int col, int row)
{
    return s_image && s_image_w > 0 && s_image_h > 0 &&
        col >= s_image_rect.x && row >= s_image_rect.y &&
        col < s_image_rect.x + s_image_rect.w && row < s_image_rect.y + s_image_rect.h;
}

void ls_tui_glass(int col, int row)
{
    const int lx = col, ly = row;
    col += s_org_x;
    row += s_org_y;
    if (!s_glass_back || !s_back || col < 0 || row < 0 || col >= s_cols || row >= s_rows) return;
    const size_t i = (size_t)row * s_cols + col;
    glass_set(s_glass_back, i, true);
    /* The cell as it will be in the grid: from the surface being drawn. */
    if (s_org_sf && lx >= 0 && ly >= 0 && lx < s_org_sf->w && ly < s_org_sf->h)
        s_glass_rec[i] = s_org_sf->back[(size_t)ly * s_org_sf->w + lx];
    else
        s_glass_rec[i] = s_back[i];
}

void ls_tui_put_glass(tui_surface *sf, tui_rect clip, int x, int y, int16_t ch, uint8_t attr)
{
    if (x < clip.x || y < clip.y || x >= clip.x + clip.w || y >= clip.y + clip.h) return;
    if(!tui_rect_contains(tui_surface_rect(sf),x,y)) return;
    tui_cell *cell=&sf->back[y*sf->w+x];
    /* Glass shows what is under it. Over the picture that is the picture;
       anywhere else it is the ground already drawn in the cell, so a ring or
       a route over a character map keeps the map's colour behind it instead
       of a black block. */
    if(!image_cell(x+s_org_x,y+s_org_y)) attr=TUI_ATTR(TUI_ATTR_FG(attr),TUI_ATTR_BG(cell->attr));
    *cell=(tui_cell){ch,attr};
    ls_tui_glass(x, y);
}

static bool glass_live(int col, int row, size_t i)
{
    return glass_bit(s_glass_back, i) && image_cell(col, row) &&
           s_back[i].ch == s_glass_rec[i].ch && s_back[i].attr == s_glass_rec[i].attr;
}

static bool image_changed(int col, int row)
{
    if (s_image_shifted && image_cell(col, row)) {
        /* image_shift also copied the exact new pixel band. */
        return false;
    }
    if (s_image_ring || !s_image_previous_valid || !image_cell(col, row)) return true;
    const int x = col - s_image_rect.x, y = row - s_image_rect.y;
    const int x0 = x * s_image_w / s_image_rect.w;
    const int x1 = ((x + 1) * s_image_w + s_image_rect.w - 1) / s_image_rect.w;
    const int y0 = y * s_image_h / s_image_rect.h;
    const int y1 = ((y + 1) * s_image_h + s_image_rect.h - 1) / s_image_rect.h;
    for (int r = y0; r < y1; r++) {
        const size_t off = (size_t)r * s_image_w + x0;
        if (memcmp(s_image + off, s_image_previous + off, (size_t)(x1 - x0) * sizeof(uint16_t)))
            return true;
    }
    return false;
}

/* THE BAR ROWS SIT IN A TALLER BAND THAN THEY ARE.

   paint_bar_ends floods the margin outside the grid with the bar's colour so
   the bar meets the glass instead of floating a cell inside it. That makes
   the painted top bar s_oy + s_ch tall - 38 px against a 17 px cell on this
   panel - with the text in the bottom 17, and the bottom bar the same the
   other way up.

   The grid itself cannot move: ls_tui_corner_inset picks s_oy so the corner
   CELLS clear the rounded arc, and that has its own suite. So the two bar
   rows, and only those, rasterise centred in the band painted for them, and
   paint_bar_ends fills the rest of that band FULL WIDTH rather than only at
   the ends. Getting that second half wrong is what left an unpainted strip
   under the text the first time. */
static int bar_row_offset(int row)
{
    /* Top bar: the band is 0 .. s_oy + s_ch, so a centred cell starts at
       s_oy / 2 instead of s_oy. */
    if (row == 0) return -(s_oy / 2);
    /* Bottom bar: the band runs from the row's own top to the glass, so its
       spare height is whatever margin sits below the grid. Portrait has no
       hint row and paint_bar_ends leaves its last row alone, so this only
       applies where a bottom bar is actually painted. */
    if (s_landscape && row == s_rows - 1) {
        const int below = s_screen_h - s_oy - s_rows * s_ch;
        return below / 2;
    }
    return 0;
}

/* The pixel rows the bar row's own cell occupies, after the offset. */
static void bar_text_span(int row, int *top, int *bottom)
{
    const int y = s_oy + row * s_ch + bar_row_offset(row);
    *top = y;
    *bottom = y + s_ch;
}

static void blit_image_cell(uint16_t *fb, int native_w, int col, int row,
                            int x0, int y0)
{
    s_image_copy_bytes += (uint32_t)s_cw * s_ch * sizeof(uint16_t);
    /* Full-image paints used the scaled sampler even when source pixels
       already matched the panel. The same divides/modulos were repeated for
       every cell of RF/audio waterfalls and native map images. Keep exact
       pixel coordinates here; the scaled path below remains the fallback. */
    if (s_image_w == s_image_rect.w * s_cw && s_image_h == s_image_rect.h * s_ch) {
        int nx = s_cw < 32 ? s_cw : 32, ny = s_ch < 32 ? s_ch : 32;
        if (nx > s_screen_w - x0) nx = s_screen_w - x0;
        if (ny > s_screen_h - y0) ny = s_screen_h - y0;
        int sx = (col - s_image_rect.x) * s_cw + s_image_x0;
        int sy = (row - s_image_rect.y) * s_ch + s_image_y0;
        if (sx >= s_image_w) sx -= s_image_w;
        if (sy >= s_image_h) sy -= s_image_h;
        if (s_landscape) {
            for (int x = 0; x < nx; x++) {
                uint16_t *run = fb + landscape_index(x0 + x, y0, native_w);
                int source_x = sx + x;
                if (source_x >= s_image_w) source_x -= s_image_w;
                const uint16_t *src = s_image + (size_t)sy * s_image_w + source_x;
                const int first = ny < s_image_h - sy ? ny : s_image_h - sy;
                for (int y = 0; y < first; y++)
                    run[s_cw_rotation ? y : -y] = src[(size_t)y * s_image_w];
                src = s_image + source_x;
                for (int y = first; y < ny; y++)
                    run[s_cw_rotation ? y : -y] = src[(size_t)(y - first) * s_image_w];
            }
        } else if (nx > 0) {
            const int first = nx < s_image_w - sx ? nx : s_image_w - sx;
            for (int y = 0; y < ny; y++) {
                int source_y = sy + y;
                if (source_y >= s_image_h) source_y -= s_image_h;
                uint16_t *run = fb + (size_t)(y0 + y) * native_w + x0;
                const uint16_t *src = s_image + (size_t)source_y * s_image_w;
                memcpy(run, src + sx, (size_t)first * sizeof(*run));
                if (first < nx) memcpy(run + first, src, (size_t)(nx - first) * sizeof(*run));
            }
        }
        return;
    }
    {
        const int iw = s_image_rect.w * s_cw, ih = s_image_rect.h * s_ch;
        /* Source coordinates once per cell, not a divide per pixel, and the
           inner loop along the framebuffer's contiguous axis. */
        /* 128 bytes: this runs on a 6 KB internal stack, and the largest
           face's cell is 15x26. */
        uint16_t sxs[32], sys[32];
        const int nx = s_cw < 32 ? s_cw : 32, ny = s_ch < 32 ? s_ch : 32;
        /* A ring's picture starts at column x0, row y0 of the source. */
        for (int x = 0; x < nx; x++)
            sxs[x] = (uint16_t)((((col - s_image_rect.x) * s_cw + x) * s_image_w / iw +
                                 s_image_x0) % s_image_w);
        for (int y = 0; y < ny; y++)
            sys[y] = (uint16_t)((((row - s_image_rect.y) * s_ch + y) * s_image_h / ih +
                                 s_image_y0) % s_image_h);
        if (s_landscape) {
            for (int x = 0; x < nx && x0 + x < s_screen_w; x++) {
                uint16_t *run = fb + landscape_index(x0+x,y0,native_w);
                const uint16_t *src = s_image + sxs[x];
                for (int y = 0; y < ny && y0 + y < s_screen_h; y++)
                    run[s_cw_rotation ? y : -y] = src[(size_t)sys[y] * s_image_w];
            }
        } else {
            for (int y = 0; y < ny && y0 + y < s_screen_h; y++) {
                uint16_t *run = fb + (uint32_t)(y0 + y) * native_w + x0;
                const uint16_t *src = s_image + (size_t)sys[y] * s_image_w;
                for (int x = 0; x < nx && x0 + x < s_screen_w; x++)
                    run[x] = src[sxs[x]];
            }
        }
    }
}

/* Coverage, 0 to 15, of one cell's glyph at every pixel of the cell. The
   same shapes blit_block and the font path paint, but as a mask, so glass
   can put ink and a halo over whatever the image left there. */
EXT_RAM_BSS_ATTR static uint8_t s_cov[32 * 32];

static bool glass_coverage(uint16_t ch, bool *halo)
{
    const int cw = s_cw < 32 ? s_cw : 32, chh = s_ch < 32 ? s_ch : 32;
    memset(s_cov, 0, sizeof(s_cov));
    *halo = true;
    if(ch>=0xff00) ch=(uint8_t)ch;
    if(ch>=0x2800 && ch<=0x28ff) {
        for(int y=0;y<chh;y++) for(int x=0;x<cw;x++)
            s_cov[y*32+x]=ls_braille_pixel(ch,x,y,cw,chh)?15:0;
        return true;
    }
    if(ch>=0x2000 && ls_box_is(ch)) {
        ls_box_mask_t m;
        ls_box_mask(ch,cw,chh,&m);
        for(int y=0;y<chh;y++) for(int x=0;x<cw;x++)
            s_cov[y*32+x]=(m.r[y]>>x&1)?15:0;
        return true;
    }
    /* A Cyrillic letter has a byte the blocks would otherwise claim. */
    const ls_font_t *face = s_font;
    const int slot = tui_cyr_slot(ch);
    if (slot >= 0) {
        face = ls_font_cyr_page(s_font);
        ch = (uint16_t)slot;
        if (!face) return false;
        goto glyph;
    }
    if (ch == ' ') return false;
    if (ch >= 0xC0) {
        const int hw = cw / 2;
        for (int sy = 0; sy < 3; sy++)
            for (int sx = 0; sx < 2; sx++) {
                if (!(ch & (1u << (sy * 2 + sx)))) continue;
                const int ya = chh * sy / 3, yb = chh * (sy + 1) / 3;
                const int xa = sx ? hw : 0, xb = sx ? cw : hw;
                for (int y = ya; y < yb; y++) memset(s_cov + y * 32 + xa, 15, (size_t)(xb - xa));
            }
        return true;
    }
    if (ch >= 0xA0 && ch <= 0xA7) {
        int h = chh * ((ch - 0xA0) + 1) / 8;
        if (h < 1) h = 1;
        const int cx = cw / 2 - 1;
        for (int y = chh - h; y < chh; y++) { s_cov[y * 32 + cx] = 15; if (cx + 1 < cw) s_cov[y * 32 + cx + 1] = 15; }
        return true;
    }
    if (ch >= 0x90 && ch <= 0x93) {
        static const uint8_t mix[4] = { 4, 8, 12, 15 };
        for (int y = 0; y < chh; y++) memset(s_cov + y * 32, mix[ch - 0x90], (size_t)cw);
        *halo = false;
        return true;
    }
    if (ch >= 0x80 && ch <= 0x8F) {
        const int hw = cw / 2, hh = chh / 2;
        for (int q = 0; q < 4; q++) {
            if (!(ch & (1u << q))) continue;
            const int xa = (q & 1) ? hw : 0, xb = (q & 1) ? cw : hw;
            const int ya = (q & 2) ? hh : 0, yb = (q & 2) ? chh : hh;
            for (int y = ya; y < yb; y++) memset(s_cov + y * 32 + xa, 15, (size_t)(xb - xa));
        }
        return true;
    }
glyph:;
    const ls_font_glyph_t *dsc = ls_font_glyph(face, (uint8_t)ch);
    if (!dsc) return false;
    const uint8_t *bmp = &face->bitmap[dsc->bitmap_index];
    const int gx = dsc->ofs_x;
    const int gy = (s_ch - face->base_line) - dsc->box_h - dsc->ofs_y;
    for (int y = 0; y < dsc->box_h; y++) {
        const int py = gy + y;
        if (py < 0 || py >= chh) continue;
        for (int x = 0; x < dsc->box_w; x++) {
            const int px = gx + x;
            if (px < 0 || px >= cw) continue;
            const uint32_t bit = (uint32_t)y * dsc->box_w + x;
            uint8_t cov = (bit & 1) ? (bmp[bit >> 1] & 0x0F) : (bmp[bit >> 1] >> 4);
            if (s_draw_crisp) cov = cov >= 8 ? 15 : 0;
            s_cov[py * 32 + px] = cov;
        }
    }
    return true;
}

bool ls_tui_glass_mask(uint16_t glyph, uint8_t *mask, bool *halo)
{
    bool ink = glass_coverage(glyph, halo);
    for (int y = 0; y < s_ch; y++)
        for (int x = 0; x < s_cw; x++)
            mask[y * s_cw + x] = ink && y < 32 && x < 32 ? s_cov[y * 32 + x] : 0;
    return ink;
}

/* A glyph over the image: the picture shows through, the glyph is inked in
   its foreground, and a one pixel halo in its background colour keeps it
   readable over any part of the map. */
static void blit_glass(uint16_t *fb, int native_w, int col, int row,
                       int x0, int y0, const tui_cell *cell)
{
    blit_image_cell(fb, native_w, col, row, x0, y0);
    bool halo;
    if (!glass_coverage(cell->ch, &halo)) return;
    const uint16_t fg = attr_fg(cell->attr), bg = attr_bg(cell->attr);
    const int cw = s_cw < 32 ? s_cw : 32, chh = s_ch < 32 ? s_ch : 32;
    for (int y = 0; y < chh; y++) {
        const int py = y0 + y;
        if (py < 0 || py >= s_screen_h) continue;
        for (int x = 0; x < cw; x++) {
            const int px = x0 + x;
            if (px < 0 || px >= s_screen_w) continue;
            const uint8_t c = s_cov[y * 32 + x];
            uint8_t near = 0;
            if (!c && halo) {
                for (int dy = -1; dy <= 1 && near < 8; dy++) {
                    const int yy = y + dy;
                    if (yy < 0 || yy >= chh) continue;
                    for (int dx = -1; dx <= 1; dx++) {
                        const int xx = x + dx;
                        if (xx < 0 || xx >= cw) continue;
                        if (s_cov[yy * 32 + xx] > near) near = s_cov[yy * 32 + xx];
                    }
                }
                if (near < 6) continue;
            } else if (!c) continue;
            const uint32_t idx = s_landscape
                ? landscape_index(px,py,native_w)
                : (uint32_t)py * native_w + px;
            fb[idx] = c ? blend565(fg, fb[idx], (uint8_t)(c * 17))
                        : blend565(bg, fb[idx], 200);
        }
    }
}

static void blit_cell(uint16_t *fb, int native_w, int native_h,
                      int col, int row, const tui_cell *cell, bool glass)
{
    (void)native_h;
    const int x0 = s_ox + col * s_cw;
    const int y0 = s_oy + row * s_ch + bar_row_offset(row);
    const uint16_t fg = attr_fg(cell->attr), bg = attr_bg(cell->attr);

    if (cell->ch < 0x100 && (uint8_t)cell->ch == (uint8_t)LS_TUI_IMAGE_CELL && image_cell(col, row)) {
        blit_image_cell(fb, native_w, col, row, x0, y0);
        return;
    }
    if (glass) {
        blit_glass(fb, native_w, col, row, x0, y0, cell);
        return;
    }

    /* A Cyrillic letter comes from its face's Cyrillic page (tui_cyr.h);
       its byte is one the blocks below would otherwise claim. */
    const ls_font_t *face = s_font;
    const ls_font_glyph_t *dsc;
    const int slot = tui_cyr_slot(cell->ch);
    if (slot >= 0) {
        face = ls_font_cyr_page(s_font);
        dsc = face ? ls_font_glyph(face, (uint8_t)slot) : NULL;
    } else {
        /* Blocks paint the whole cell themselves, background included. */
        if (blit_block(fb, native_w, x0, y0, cell->ch, fg, bg)) return;
        dsc = ls_font_glyph(s_font, (uint8_t)cell->ch);
    }
    const uint8_t *bmp = dsc ? &face->bitmap[dsc->bitmap_index] : NULL;
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
            uint16_t *run = fb + landscape_run(px,y0,n,native_w);
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
    const int gy = y0 + (s_ch - face->base_line) - dsc->box_h - dsc->ofs_y;

    /* Outer loop across the framebuffer's stride, inner along it: rows in
       portrait, logical columns in landscape. */
    const int outer_n = s_landscape ? dsc->box_w : dsc->box_h;
    const int inner_n = s_landscape ? dsc->box_h : dsc->box_w;
    for (int o = 0; o < outer_n; o++) {
        for (int i = 0; i < inner_n; i++) {
            const int x = s_landscape ? o : i, y = s_landscape ? i : o;
            int py = gy + y;
            if (py < 0 || py >= s_screen_h) continue;
            int px = gx + x;
            if (px < 0 || px >= s_screen_w) continue;
            uint32_t bit = (uint32_t)y * dsc->box_w + x;
            /* 4 bpp, high nibble first, exactly as lv_font_conv emits it. */
            uint8_t cov = (bit & 1) ? (bmp[bit >> 1] & 0x0F) : (bmp[bit >> 1] >> 4);
            if(s_draw_crisp)cov=cov>=8?15:0;
            if (!cov) continue;
            uint32_t idx = s_landscape
                ? landscape_index(px,py,native_w)
                : (uint32_t)py * native_w + px;
            fb[idx] = s_ramp[cov];
        }
    }
}

/* The look, applied where it is safe to apply it: at the top of a
   present or in begin, on the task that owns the framebuffer. The palette
   the blitter indexes, the ramp built from it and the front grid all change
   together here, so no cell can be marked drawn in colours that have gone. */
LS_COLD static void look_apply(void)
{
    s_look_dirty = false;
    s_draw = ls_tui_theme_effective(s_theme, s_daylight);
    s_draw_crisp = s_crisp_text;
    memset(s_ramp_valid, 0, sizeof(s_ramp_valid));
    ls_tui_invalidate();
}

/* Native rows written since the last present. The cache writeback that makes
   pixels visible costs in proportion to the rows handed to it, so a present
   hands over only these. In landscape a native row is a logical column. */
static int s_dirty_lo = INT32_MAX, s_dirty_hi = -1;
/* Native rows, including both rotations. Oversized hosts use the range
   fallback; the P4 needs only 1232 bits. No per-frame allocation. */
static uint8_t s_dirty_rows[2048 / 8];

static void dirty_logical(int x, int y, int w, int h)
{
    int lo, hi;
    if (s_landscape && s_cw_rotation) { lo = s_screen_w - (x + w); hi = s_screen_w - 1 - x; }
    else if (s_landscape) { lo = x; hi = x + w - 1; }
    else             { lo = y; hi = y + h - 1; }
    if (lo < s_dirty_lo) s_dirty_lo = lo;
    if (hi > s_dirty_hi) s_dirty_hi = hi;
    for (int r = lo < 0 ? 0 : lo; r <= hi && r < 2048; r++)
        s_dirty_rows[r >> 3] |= (uint8_t)(1u << (r & 7));
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
    dirty_logical(x, y, w, h);
    const uint32_t c2 = ((uint32_t)c << 16) | c;
    if (s_landscape) {
        for (int px = x; px < x + w; px++)
            fill_run(fb + landscape_run(px,y,h,native_w),
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

/* Bars reach the physical rounded edge; text remains inside corner_pad.
   Only the empty end caps and the margin outside them are painted here,
   never a word or a touch target.

   THIS USED TO BE LANDSCAPE ONLY, and portrait is the posture this board is
   held in. The grid stands its text off the corners - corner_pad is five
   cells at the top of a 72 px radius - and nothing put the bar back behind
   that standoff, so the status bar began fifty pixels in from the glass and
   the whole bar read as inset. The complaint is always that the border has
   been brought inside; what had been brought inside is the background, and
   the fix is to paint it back out rather than to move the text.

   The margin band outside the bar's own row goes with it. Filling the ends
   but not the strip above them leaves the bar floating a cell below the top
   edge, which is the same defect one axis over. */
/* What each end was last painted with, and whether the grid redrew any cell
   of that bar row since. The ends only change when one of those does, and in
   landscape repainting them is a scatter of single pixels across the whole
   panel, so an unchanged end is left alone. */
static uint32_t s_bar_sig[2] = { UINT32_MAX, UINT32_MAX };
static bool     s_bar_row_drawn[2];

static void paint_bar_ends(uint16_t *fb,int native_w)
{
    for(int end=0;end<2;end++) {
        /* The status bar is drawn in both postures. The hint bar is not -
           portrait has no hint row, and its last row belongs to whatever
           screen is up - so the bottom end stays gated. s_landscape is the
           conservative test for it: a panel wide enough for a hint row is
           not necessarily landscape, but a landscape panel always has one,
           so this can miss a bleed and can never bleed a screen's own row. */
        if(end && !s_landscape) continue;
        const int row=end?s_rows-1:0;
        const int pad=ls_tui_corner_pad(row);
        const int left=s_ox+pad*s_cw;
        const int right=s_ox+(s_cols-pad)*s_cw;
        const uint8_t attr=s_back[(size_t)row*s_cols+pad].attr;
        /* Arbitrary surfaces (including pixel/touch calibration) do not have
           router bars. Their edge CELLS are never touched - but the margin
           outside the grid still has to be put back, because the last screen
           may have left this strip painted its bar colour and nothing else
           repaints the margin between look changes. */
        const bool is_bar=TUI_ATTR_BG(attr)==(end?(TUI_BLACK|TUI_BRIGHT):TUI_CYAN);
        const uint16_t color=is_bar?attr_bg(attr):PALETTE[0];
        const uint32_t sig=((uint32_t)color<<16)|((uint32_t)is_bar<<15)|
                           ((uint32_t)(pad&0x7F)<<8)|(uint32_t)(s_corner_r&0xFF);
        if(sig==s_bar_sig[end] && !s_bar_row_drawn[end]) continue;
        s_bar_sig[end]=sig;
        s_bar_row_drawn[end]=false;
        const int band_top=s_oy+row*s_ch, band_bottom=s_oy+(row+1)*s_ch;
        /* Out to the glass on the bar's own side, and no further: the middle
           of the panel is the grid's. */
        const int y0=end?band_top:0;
        const int y1=end?s_screen_h:band_bottom;
        for(int y=y0;y<y1;y++) {
            const int inset=ls_tui_row_inset(y,s_screen_h,s_corner_r);
            int text_top, text_bottom;
            bar_text_span(row, &text_top, &text_bottom);
            if(y>=text_top && y<text_bottom) {
                if(!is_bar) continue;
                /* The grid has already drawn the middle of this row, text
                   and all. Only the ends are ours. */
                if(left>inset) fill_logical(fb,native_w,inset,y,left-inset,1,color);
                if(s_screen_w-inset>right)
                    fill_logical(fb,native_w,right,y,s_screen_w-inset-right,1,color);
            } else if(s_screen_w-2*inset>0) {
                /* Outside the grid entirely, so the bar takes the lot. */
                fill_logical(fb,native_w,inset,y,s_screen_w-2*inset,1,color);
            }
        }
    }
}

LS_COLD bool ls_tui_begin(int screen_w, int screen_h)
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
    /* "Landscape" here means turned a quarter from the glass: the logical
       size is the native size swapped. Judging it by w > h alone took a
       landscape-native panel (800x480) at rotation 0 for a turned one, and
       the turned blit then wrote rows far past its framebuffer. */
    s_landscape = s_screen_w != s_screen_h && s_screen_w == fb.height && s_screen_h == fb.width;
    tell_orientation();
    /* The chosen font becomes THE font here and nowhere else.

       This is the one moment the grid dimensions are settled, so it is the
       one moment the choice can take effect. Setting the index at any other
       time changes what the next session will build and nothing about this
       one, which is why changing it asks for a restart. */
    ls_tui_set_font(ls_tui_font_at(ls_tui_font_index()));
    s_cw = s_font->cell_w;
    s_ch = s_font->cell_h;

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
    s_glass_back  = heap_caps_calloc((cells + 7) / 8, 1, MALLOC_CAP_SPIRAM);
    s_glass_front = heap_caps_calloc((cells + 7) / 8, 1, MALLOC_CAP_SPIRAM);
    s_glass_rec   = heap_caps_calloc(cells, sizeof(tui_cell), MALLOC_CAP_SPIRAM);
    s_base_rec = heap_caps_calloc(cells,sizeof(tui_cell),MALLOC_CAP_SPIRAM);
    s_base_front = heap_caps_calloc((cells+7)/8,1,MALLOC_CAP_SPIRAM);
    if (!s_back || !s_front || !s_glass_back || !s_glass_front || !s_glass_rec || !s_base_rec || !s_base_front) {
        ls_tui_end();
        return false;
    }

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
        if (!fb.pixels) { /* virtual panel: cells only until an on-demand paint */ }
        else if (g == 0) memset(fb.pixels, 0, n * sizeof(uint16_t));
        else        fill_run(fb.pixels, (int)n, g, ((uint32_t)g << 16) | g);
    }
    ls_panel_fb_present();

    /* Optional in renderer-only builds. Retain waterfall storage across visits. */
    extern void ls_wf_prepare(int, int) __attribute__((weak));
    if (ls_wf_prepare) ls_wf_prepare(screen_w, screen_h);
    tui_surface_setup(&s_surface, s_back, s_front, s_cols, s_rows);
    ESP_LOGI(TAG, "grid %dx%d cells, %s at %dx%d px, inset %d,%d, %s",
             s_cols, s_rows, s_font->name, s_cw, s_ch, s_ox, s_oy,
             s_landscape ? "landscape" : "portrait");
    return true;
}

LS_COLD void ls_tui_end(void)
{
    /* The marks first: begin calls this after the grid size has already
       changed, and clearing them at the new size would overrun the old. */
    free(s_glass_back);  s_glass_back = NULL;
    free(s_glass_front); s_glass_front = NULL;
    free(s_glass_rec);   s_glass_rec = NULL;
    free(s_base_rec); s_base_rec=NULL;
    free(s_base_front); s_base_front=NULL;
    s_base_rect=tui_rect_make(0,0,0,0);s_cell_palette=NULL;
    ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
    free(s_back);  s_back = NULL;
    free(s_front); s_front = NULL;
    memset(&s_surface, 0, sizeof(s_surface));
}

tui_surface *ls_tui_surface(void) { return s_back ? &s_surface : NULL; }

void ls_tui_remote_geometry(void)
{
    LS_PRINTF(t_ls_tui_hubscreen_width_d_height, "HUBSCREEN width=%d height=%d x=%d y=%d cols=%d rows=%d cw=%d ch=%d\n",
           s_screen_w, s_screen_h, s_ox, s_oy, s_cols, s_rows, s_cw, s_ch);
}

void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch)
{
    if (cols) *cols = s_cols;
    if (rows) *rows = s_rows;
    if (cw)   *cw = s_cw;
    if (ch)   *ch = s_ch;
}

/* No glass (a Nano whose screen is a terminal): the cells are the screen,
   and turning them into pixels nobody sees cost a game 5 ms a frame. */
static bool s_pixels_off;
void ls_tui_set_pixels_off(bool off)
{
    if (s_pixels_off && !off) {
        s_pixels_off = false;
        ls_tui_invalidate();
        s_look_dirty = true;            /* the margin too: nothing was painted */
        return;
    }
    s_pixels_off = off;
}
bool ls_tui_pixels_off(void) { return s_pixels_off; }

/* 'tui cost': how often the whole grid was thrown away, and by whom last. */
static uint32_t s_inval_n;
static void *s_inval_by;
void ls_tui_invalidate_stats(uint32_t *n, void **by) { *n = s_inval_n; *by = s_inval_by; s_inval_n = 0; }

void ls_tui_invalidate(void)
{
    s_inval_n++;
    s_inval_by = __builtin_return_address(0);
    /* A cell that cannot occur forces every comparison to differ. */
    if (s_front) memset(s_front, 0xFF, (size_t)s_cols * s_rows * sizeof(tui_cell));
    s_bar_sig[0] = s_bar_sig[1] = UINT32_MAX;
}

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
    return ls_tui_grid_corner_cells(s_screen_w, s_screen_h, s_cw, s_ch,
                                    s_grid_r, s_ox, s_oy, s_cols, s_rows, row);
}

void ls_tui_set_cutout(int cx, int cy, int radius)
{
    s_cutout_cx = cx;
    s_cutout_cy = cy;
    s_cutout_r = radius > 0 ? radius : 0;
}

bool ls_tui_cutout(tui_rect *cells)
{
    int x = 0, y = 0, w = 0, h = 0, lx, ly;
    bool hit = false;
    if (s_back && s_cutout_r > 0) {
        ls_tui_native_to_logical(s_cutout_cx, s_cutout_cy,
                                 s_landscape ? (s_cw_rotation ? 1 : -1) : 0,
                                 s_screen_w, s_screen_h, &lx, &ly);
        hit = ls_tui_cutout_cells(lx, ly, s_cutout_r, s_ox, s_oy, s_cw, s_ch,
                                  s_cols, s_rows, bar_row_offset(0),
                                  bar_row_offset(s_rows - 1),
                                  &x, &y, &w, &h);
    }
    if (cells) *cells = tui_rect_make(x, y, w, h);
    return hit;
}

void ls_tui_set_rotation_cw(bool clockwise)
{
    s_cw_rotation = clockwise;
}

static void (*s_orientation_fn)(int ux, int uy);
void ls_tui_set_orientation_listener(void (*fn)(int ux, int uy))
{
    s_orientation_fn = fn;
}
/* landscape_index: clockwise puts logical up at the panel's native left
   edge, counter-clockwise at its right edge. */
static void tell_orientation(void)
{
    if (!s_orientation_fn) return;
    if (!s_landscape) s_orientation_fn(0, 1);
    else s_orientation_fn(s_cw_rotation ? -1 : 1, 0);
}

/* Takes effect on the next begin(): the grid dimensions come from the
   cell size, so the buffers have to be reallocated to change it. */
/* Which font is current. The list of them, and which one is wanted,
   is in ls_font_list.c: this file owns the framebuffer and cannot come to
   the host bench, and the SETTINGS screen that offers the choice can. */
void ls_tui_set_font(const ls_font_t *font) { if (font) s_font = font; }
void ls_tui_set_crisp_text(bool on) {if(s_crisp_text!=on){s_crisp_text=on;s_look_dirty=true;}}
bool ls_tui_crisp_text(void) {return s_crisp_text;}

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
    cells = org_rect(cells);
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
    cells = org_rect(cells);

    if (cells.x != s_reserved.x || cells.y != s_reserved.y ||
        cells.w != s_reserved.w || cells.h != s_reserved.h) return;

    ls_panel_fb_t fb;
    if (!ls_panel_fb(&fb) || !fb.pixels) return;

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
                            landscape_index(px,y0,fb.width);
            const uint16_t *col = src + x;
            for (int y = 0; y < h; y++) run[s_cw_rotation ? y : -y] = col[(size_t)y * src_w];
        }
    } else {
        for (int y = 0; y < h; y++) {
            uint16_t *run = fb.pixels + (uint32_t)(y0 + y) * fb.width + x0;
            memcpy(run, src + (size_t)y * src_w, (size_t)w * sizeof(uint16_t));
        }
    }
    if (s_landscape && s_cw_rotation) ls_panel_fb_present_rows(s_screen_w - (x0 + w), s_screen_w - x0);
    else if (s_landscape) ls_panel_fb_present_rows(x0, x0 + w);
    else             ls_panel_fb_present_rows(y0, y0 + h);
}

/* The worst frame since the last look, and where its time went: a full
   repaint lasts one frame, so the last frame alone never shows it. */
static struct { uint32_t us, sync_us, ramps; int cells; } s_peak;


void ls_tui_peak_cost(uint32_t *us, int *cells, uint32_t *sync_us, uint32_t *ramps)
{
    if (us)      *us = s_peak.us;
    if (cells)   *cells = s_peak.cells;
    if (sync_us) *sync_us = s_peak.sync_us;
    if (ramps)   *ramps = s_peak.ramps;
    memset(&s_peak, 0, sizeof(s_peak));
}

/* A RING PICTURE THAT MOVED IS MOVED ON THE GLASS, NOT DRAWN AGAIN.

   A scrolling waterfall changes every pixel of its picture every frame, and
   drawing all of its cells from the source cost 27 ms a present on the P4.
   Moving the pixels already in the framebuffer is one copy per line of the
   picture; then only the new pixel band is copied from the source. That is
   only right when the glass holds exactly the last picture: same source,
   size and cells, every cell of it an image cell now and at the last
   present, 1:1 pixels, and nothing reserved or painted over it. Anything
   else draws every cell, as before. */
static bool image_can_shift(void)
{
    if (!s_image_ring || !s_image || (!s_image_dx && !s_image_dy) || (s_image_dx && s_image_dy)) return false;
    const tui_rect r = s_image_rect;
    if (!s_image_shown || s_image_shown_src != s_image || s_image_shown_w != s_image_w ||
        s_image_shown_h != s_image_h || memcmp(&s_image_shown_rect, &r, sizeof(r))) return false;
    if (s_image_w != r.w * s_cw || s_image_h != r.h * s_ch) return false;
    if (s_image_dx < 0 || s_image_dy < 0 ||
        s_image_dx >= s_image_w || s_image_dy >= s_image_h) return false;
    if (r.x < 0 || r.y < 1 || r.x + r.w > s_cols || r.y + r.h > s_rows - 1) return false;
    for (int row = r.y; row < r.y + r.h; row++)
        for (int col = r.x; col < r.x + r.w; col++) {
            const size_t i = (size_t)row * s_cols + col;
            if (reserved_cell(col, row) || glass_bit(s_glass_back, i) || glass_bit(s_glass_front, i))
                return false;
            if (s_back[i].ch >= 0x100 || (uint8_t)s_back[i].ch != (uint8_t)LS_TUI_IMAGE_CELL ||
                s_front[i].ch >= 0x100 || (uint8_t)s_front[i].ch != (uint8_t)LS_TUI_IMAGE_CELL)
                return false;
        }
    return true;
}

/* Account at each actual copy site, so traffic tests catch extra copies. */
static inline void image_copy_run(uint16_t *dst, const uint16_t *src, size_t bytes)
{
    memmove(dst, src, bytes);
    s_image_copy_bytes += (uint32_t)bytes;
}

static void image_shift(uint16_t *fb, int native_w)
{
    const int X0 = s_ox + s_image_rect.x * s_cw, Y0 = s_oy + s_image_rect.y * s_ch;
    const int W = s_image_w, H = s_image_h, dx = s_image_dx, dy = s_image_dy;
    if (dx) {
        if (s_landscape) {
            /* A logical column is a native row: whole runs, oldest first. */
            for (int x = X0; x < X0 + W - dx; x++)
                image_copy_run(fb + landscape_run(x, Y0, H, native_w),
                       fb + landscape_run(x + dx, Y0, H, native_w), (size_t)H * sizeof(uint16_t));
        } else {
            for (int y = Y0; y < Y0 + H; y++)
                image_copy_run(fb + (size_t)y * native_w + X0, fb + (size_t)y * native_w + X0 + dx,
                        (size_t)(W - dx) * sizeof(uint16_t));
        }
    } else if (s_landscape) {
        /* Down the picture is along a native row: a move inside each run. */
        for (int x = X0; x < X0 + W; x++) {
            uint16_t *run = fb + landscape_run(x, Y0, H, native_w);
            if (s_cw_rotation) image_copy_run(run + dy, run, (size_t)(H - dy) * sizeof(uint16_t));
            else               image_copy_run(run, run + dy, (size_t)(H - dy) * sizeof(uint16_t));
        }
    } else {
        for (int y = Y0 + H - 1; y >= Y0 + dy; y--)
            image_copy_run(fb + (size_t)y * native_w + X0, fb + (size_t)(y - dy) * native_w + X0,
                   (size_t)W * sizeof(uint16_t));
    }
    /* The retained band moved once above. Copy only the new pixels, not
       whole cells (a two-row update formerly recopied a 17-row cell). */
    const int nx = dx ? W - dx : 0, ny = 0;
    const int nw = dx ? dx : W, nh = dx ? H : dy;
    if (s_landscape) {
        for (int x = nx; x < nx + nw; x++) {
            uint16_t *dst = fb + landscape_index(X0 + x, Y0 + ny, native_w);
            const int sx = (x + s_image_x0) % W;
            for (int y = ny; y < ny + nh; y++)
                dst[s_cw_rotation ? y - ny : ny - y] =
                    s_image[(size_t)((y + s_image_y0) % H) * W + sx];
            s_image_copy_bytes += (uint32_t)nh * sizeof(uint16_t);
        }
    } else {
        for (int y = ny; y < ny + nh; y++) {
            uint16_t *dst = fb + (size_t)(Y0 + y) * native_w + X0 + nx;
            const uint16_t *src = s_image + (size_t)((y + s_image_y0) % H) * W;
            const int sx = (nx + s_image_x0) % W;
            const int first = nw < W - sx ? nw : W - sx;
            image_copy_run(dst, src + sx, (size_t)first * sizeof(uint16_t));
            if (first < nw) image_copy_run(dst + first, src, (size_t)(nw - first) * sizeof(uint16_t));
        }
    }
    dirty_logical(X0, Y0, W, H);
}

/* No bit set in [start, start + n). */
static bool bits_clear(const uint8_t *bits, size_t start, size_t n)
{
    if (!bits) return true;
    size_t i = start, end = start + n;
    for (; i < end && (i & 7); i++) if (glass_bit(bits, i)) return false;
    for (; i + 8 <= end; i += 8) if (bits[i >> 3]) return false;
    for (; i < end; i++) if (glass_bit(bits, i)) return false;
    return true;
}

static volatile uint32_t s_present_count;

uint32_t ls_tui_present_count(void) { return s_present_count; }

int ls_tui_present(void)
{
    if (!s_back || !s_front) return 0;
    ls_panel_fb_t fb;
    if (!ls_panel_fb(&fb) || (!s_pixels_off && !fb.pixels)) return 0;

    uint64_t t0 = esp_timer_get_time();
    s_image_copy_bytes = s_present_flush_bytes = 0;
    uint32_t sync_us = 0;
    const uint32_t ramps0 = s_prof_ramps;
    /* A theme or Daylight change lands here, where the palette, the
       ramp, every cell and the margin outside the grid change together. */
    bool margin = false;
    if (s_look_dirty) {
        look_apply();
        if (!s_pixels_off) {
            paint_margin(fb.pixels, fb.width);
            margin = true;
        }
    }
    s_image_shifted = !s_pixels_off && !margin && s_image_dirty && image_can_shift();
    if (s_image_shifted) image_shift(fb.pixels, fb.width);
    int drawn = 0;
    bool image_cell_invalidated = false;
    bool image_area_touched = false;
    for (int row = 0; row < s_rows; row++) {
        /* A row with the same cells and no glass, basemap or picture to
           weigh draws nothing: one compare instead of the per-cell tests,
           which cost ~2.5 ms a present on a still Nano screen. */
        const size_t r0 = (size_t)row * s_cols;
        if (!s_image_dirty && !s_base_dirty && !margin &&
            !memcmp(&s_back[r0], &s_front[r0], (size_t)s_cols * sizeof(tui_cell)) &&
            (!s_base_rec || row < s_base_rect.y || row >= s_base_rect.y + s_base_rect.h) &&
            bits_clear(s_glass_back, r0, s_cols) && bits_clear(s_glass_front, r0, s_cols) &&
            bits_clear(s_base_front, r0, s_cols))
            continue;
        for (int col = 0; col < s_cols; col++) {
            if (reserved_cell(col, row)) continue;
            size_t i = (size_t)row * s_cols + col;
            const bool glass = glass_live(col, row, i);
            const bool base=basemap_live(col,row,i);
            const bool on_base=!base && !glass && s_base_rec && glass_bit(s_glass_back,i) &&
                               tui_rect_contains(s_base_rect,col,row);
            /* s_base_front: drawn with any basemap colour, ink or ground, so a
               cell redraws when it moves onto or off the basemap's palette. */
            const bool tinted = base || on_base;
            if (s_back[i].ch == s_front[i].ch &&
                s_back[i].attr == s_front[i].attr &&
                glass == glass_bit(s_glass_front, i) &&
                tinted == glass_bit(s_base_front,i) && !(s_base_dirty && tinted) &&
                !(s_image_dirty && (glass || (s_back[i].ch < 0x100 && (uint8_t)s_back[i].ch == (uint8_t)LS_TUI_IMAGE_CELL)) &&
                  image_changed(col, row))) continue;
            s_cell_palette=base?s_base_palette:NULL;
            s_cell_bg_palette=on_base?s_base_palette:NULL;
            if (!s_pixels_off) blit_cell(fb.pixels, fb.width, fb.height, col, row, &s_back[i], glass);
            s_cell_palette=NULL;
            s_cell_bg_palette=NULL;
            /* Once the ring was shown, only a changed cell inside it can
               invalidate that fact between image updates. Avoid rescanning
               the whole waterfall on every otherwise-idle present. */
            if (s_image_ring && tui_rect_contains(s_image_rect, col, row)) {
                image_area_touched = true;
                if (s_image_shown && (glass || s_back[i].ch >= 0x100 ||
                    (uint8_t)s_back[i].ch != (uint8_t)LS_TUI_IMAGE_CELL)) {
                    s_image_shown = false;
                    image_cell_invalidated = true;
                }
            }
            glass_set(s_base_front,i,tinted);
            glass_set(s_glass_front, i, glass);
            if (!s_pixels_off)
                dirty_logical(s_ox + col * s_cw, s_oy + row * s_ch + bar_row_offset(row),
                              s_cw, s_ch);
            if (row == 0) s_bar_row_drawn[0] = true;
            if (row == s_rows - 1) s_bar_row_drawn[1] = true;
            s_front[i] = s_back[i];
            drawn++;
        }
    }
    if (s_image_dirty && s_image_previous && s_image) {
        memcpy(s_image_previous, s_image, (size_t)s_image_w * s_image_h * sizeof(uint16_t));
        s_image_previous_valid = true;
    }
    /* What the glass holds now, for the next present's move. */
    if (s_image_ring && (s_image_dirty || s_image_shifted || image_area_touched)) {
        bool all = s_image != NULL && !image_cell_invalidated;
        const tui_rect r = s_image_rect;
        /* image_can_shift already checked every cell before a successful
           move. Otherwise validate only when a new image state is presented,
           not on every clean frame while the ring remains installed. */
        if (!s_image_shifted)
            for (int row = r.y; all && row < r.y + r.h; row++)
                for (int col = r.x; col < r.x + r.w; col++) {
                    if (col < 0 || row < 0 || col >= s_cols || row >= s_rows) { all = false; break; }
                    const tui_cell c = s_front[(size_t)row * s_cols + col];
                    all = c.ch < 0x100 && (uint8_t)c.ch == (uint8_t)LS_TUI_IMAGE_CELL;
                }
        s_image_shown = all;
        s_image_shown_rect = r;
        s_image_shown_src = s_image;
        s_image_shown_w = s_image_w;
        s_image_shown_h = s_image_h;
    } else if (!s_image_ring) s_image_shown = false;
    s_image_dx = s_image_dy = 0;
    s_image_shifted = false;
    s_image_dirty = false;
    s_base_dirty = false;
    if ((drawn || margin) && !s_pixels_off) {
        if (margin) s_bar_sig[0] = s_bar_sig[1] = UINT32_MAX;
        paint_bar_ends(fb.pixels,fb.width);
    }
    if (s_dirty_hi >= s_dirty_lo) {
        const int64_t ts = esp_timer_get_time();
        if (s_dirty_lo < 0 || s_dirty_hi >= 2048) {
            ls_panel_fb_present_rows(s_dirty_lo, s_dirty_hi + 1);
            s_present_flush_bytes = (uint32_t)(s_dirty_hi - s_dirty_lo + 1) * fb.width * 2;
        } else for (int y = s_dirty_lo; y <= s_dirty_hi;) {
            if (!(s_dirty_rows[y >> 3] & (1u << (y & 7)))) { y++; continue; }
            const int start = y++;
            while (y <= s_dirty_hi && (s_dirty_rows[y >> 3] & (1u << (y & 7)))) y++;
            ls_panel_fb_present_rows(start, y);
            s_present_flush_bytes += (uint32_t)(y - start) * fb.width * 2;
        }
        memset(s_dirty_rows, 0, sizeof(s_dirty_rows));
        sync_us = (uint32_t)(esp_timer_get_time() - ts);
        s_dirty_lo = INT32_MAX;
        s_dirty_hi = -1;
    }
    s_last_us = (uint32_t)(esp_timer_get_time() - t0);
    if (s_last_us > s_max_us) s_max_us = s_last_us;
    s_last_cells = drawn;
    if (s_last_us > s_peak.us) {
        s_peak.us = s_last_us;
        s_peak.cells = drawn;
        s_peak.sync_us = sync_us;
        s_peak.ramps = s_prof_ramps - ramps0;
    }
    /* The renderer also builds without the app router in low-level tests. */
    extern void ls_tui_transition_present(uint32_t, int, uint32_t) __attribute__((weak));
    if (ls_tui_transition_present)
        ls_tui_transition_present(s_last_us, drawn, sync_us);
    /* The opening animation advances per frame that actually reached the
       panel, not per frame drawn: the app loop discards a drawn frame when a
       font change forces a regrid. This is the only place that knows. */
    extern void ls_anim_presented(void) __attribute__((weak));
    if (ls_anim_presented) ls_anim_presented();
    s_present_count++;
    return drawn;
}

/* Native pixel to grid cell. */

bool ls_tui_pixel_to_cell(int native_x, int native_y, int *col, int *row)
{
    if (!s_back) return false;
    const int nw = s_landscape ? s_screen_h : s_screen_w;
    const int nh = s_landscape ? s_screen_w : s_screen_h;
    if (native_x < 0 || native_y < 0 || native_x >= nw || native_y >= nh) return false;
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
    if (lx < 0) return false;
    int c = lx / s_cw;
    if (c >= s_cols) return false;
    /* Off the glass is still not a cell: the bands below widen what counts
       as a bar row, they do not widen the panel. */
    if (ly < 0 || ly >= s_screen_h) return false;

    /* The bar rows are rasterised centred in the taller band painted for
       them, so uniform row arithmetic would hand back a cell the pixel is
       not drawn in and put the tab strip's touch targets off from its
       labels. The whole painted band belongs to its bar row, which is what
       a thumb aiming at a bar expects anyway. */
    int r;
    int top_text, top_end;
    bar_text_span(0, &top_text, &top_end);
    const int bottom_start = s_oy + (s_rows - 1) * s_ch + bar_row_offset(s_rows - 1);
    if (ly < s_oy + s_ch) {
        r = 0;
    } else if (s_landscape && ly >= bottom_start) {
        r = s_rows - 1;
    } else {
        const int gy = ly - s_oy;
        if (gy < 0) return false;
        r = gy / s_ch;
    }
    if (r < 0 || r >= s_rows) return false;
    (void)top_text; (void)top_end;
    if (col) *col = c;
    if (row) *row = r;
    return true;
}

bool ls_tui_pixel_to_grid(int native_x, int native_y, int *gx, int *gy)
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
    if (gx) *gx = lx - s_ox;
    if (gy) *gy = ly - s_oy;
    return true;
}

/* A printable stand-in for the procedural glyphs, so a dump of a waterfall
   still looks like one. The ramp runs light to dark the way the shades do. */
static char printable(int16_t ch)
{
    if(ch>=0x2800 && ch<=0x28ff) return '#';
    if(ch>=0x2000 && ls_box_is(ch)) return ls_box_ascii(ch);
    unsigned char c = (unsigned char)ch;
    if (c >= 0x20 && c < 0x7F) return ch;
    if (c >= 0x80 && c <= 0x8F) return '#';        /* quadrant blocks */
    if (c >= 0xC0) return '#';                     /* sextants        */
    if (c == 0x90) return '.';                     /* 25% shade       */
    if (c == 0x91) return ':';                     /* 50%             */
    if (c == 0x92) return '+';                     /* 75%             */
    if (c == 0x93) return '#';                     /* full            */
    if (c >= 0xA0 && c <= 0xA7) return "_.,-=+*|"[c - 0xA0];  /* traces */
    if (c == (uint8_t)LS_TUI_IMAGE_CELL) return '.';
    return '?';
}

LS_COLD void ls_tui_dump(void)
{
    if (!s_back) { printf("tui: no grid\n"); return; }

    LS_PRINTF(t_ls_tui_tui_dx_d_cells, "tui: %dx%d cells, %s, %s\n", s_cols, s_rows,
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
        for (int x = 0; x < s_cols; x++) {
            const int16_t ch = s_back[(size_t)y * s_cols + x].ch;
            /* A Cyrillic letter as the UTF-8 it is, two bytes, one column. */
            const uint32_t cp = tui_cyr_slot_codepoint(tui_cyr_slot(ch));
            if (cp) { putchar(0xC0 | (int)(cp >> 6)); putchar(0x80 | (int)(cp & 0x3F)); }
            else putchar(printable(ch));
        }
        putchar('\n');
    }
}

uint32_t ls_tui_max_cost_take(void)
{
    uint32_t m = s_max_us; s_max_us = 0; return m;
}

void ls_tui_last_cost(uint32_t *us, int *cells)
{
    if (us)    *us = s_last_us;
    if (cells) *cells = s_last_cells;
}
