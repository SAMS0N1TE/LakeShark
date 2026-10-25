

#ifndef LS_TUI_H
#define LS_TUI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "tuilib/tui_core.h"
#include "ls_font.h"

/* Allocate the grids for a logical screen size. The caller owns the rotation,
   so pass landscape as (1232, 568) and portrait as (568, 1232); this file asks
   nothing of LVGL. Returns false when the panel is unavailable or memory is
   short. */
bool ls_tui_begin(int screen_w, int screen_h);
void ls_tui_end(void);

/* The surface widgets draw into. NULL before begin(). */
tui_surface *ls_tui_surface(void);

/* Grid size in cells, and the cell size in pixels. */
void ls_tui_geometry(int *cols, int *rows, int *cell_w, int *cell_h);

/* Diff back against front, blit the cells that differ, copy back to front.
   Returns the number of cells actually redrawn, which is the number that
   matters: it is what separates this from a full repaint. */
int ls_tui_present(void);

/* How many presents have finished, for anything that wants to line its own
   cache-off work up with the screen's frames. */
uint32_t ls_tui_present_count(void);

/* Force the next present to redraw every cell. */
void ls_tui_invalidate(void);

/* No glass (a screen that is only a terminal): present keeps the cells and
   draws no pixels. Turning pixels back on repaints everything. */
void ls_tui_set_pixels_off(bool off);
bool ls_tui_pixels_off(void);

/* Sub-cell graphics. Put one of these in a cell's `ch` and the blitter
   draws it as filled rectangles instead of looking up a glyph - cheaper than a
   character and exact at the pixel. Quadrants double both resolutions, so a
   115x27 grid plots a 230x54 image. */
#define LS_TUI_QUAD(tl, tr, bl, br)     ((char)(0x80 | ((tl) ? 1 : 0) | ((tr) ? 2 : 0) |                    ((bl) ? 4 : 0) | ((br) ? 8 : 0)))
#define LS_TUI_BLOCK_FULL   LS_TUI_QUAD(1, 1, 1, 1)
#define LS_TUI_BLOCK_UPPER  LS_TUI_QUAD(1, 1, 0, 0)
#define LS_TUI_BLOCK_LOWER  LS_TUI_QUAD(0, 0, 1, 1)
#define LS_TUI_BLOCK_LEFT   LS_TUI_QUAD(1, 0, 1, 0)
#define LS_TUI_BLOCK_RIGHT  LS_TUI_QUAD(0, 1, 0, 1)
/* Sextants: the same two colours, but two across and three down, so half
   again the vertical resolution and dots nearly square on this font. Bit
   (row * 2 + col), rows from the top. The printable stand-in is '#'. */
#define LS_TUI_SEXT(bits)   ((char)(0xC0 | ((bits) & 0x3F)))
#define LS_TUI_SHADE_25     ((char)0x90)
#define LS_TUI_SHADE_50     ((char)0x91)
#define LS_TUI_SHADE_75     ((char)0x92)
#define LS_TUI_SHADE_FULL   ((char)0x93)
#define LS_TUI_IMAGE_CELL   ((char)0xB0)
/* Caller-owned image behind IMAGE_CELL cells; normal text can cover it. */
void ls_tui_image(tui_rect cells, const uint16_t *src, int w, int h, uint32_t serial);
/* A scrolling picture kept as a ring, for a waterfall. Column x0 and row y0
   of `src` are the picture's left and top edges, and since the last call it
   has moved dx pixels left or dy pixels down (one or the other) with the new
   lines written where the ring starts over. With the same src, size and
   cells, and the picture 1:1 on the glass, a present moves the pixels already
   there and draws only the cells the new lines land in; otherwise it draws
   every image cell. */
void ls_tui_image_ring(tui_rect cells, const uint16_t *src, int w, int h,
                       int x0, int y0, int dx, int dy, uint32_t serial);
/* Whether this build draws image cells at all (false on a bench without a
   panel), so a renderer can choose cells before doing any pixel work. */
bool ls_tui_has_images(void);
/* Where the current ring picture starts in its source; 0, 0 for a plain one. */
void ls_tui_image_ring_offset(int *x0, int *y0);

/* Glass: draw this cell's glyph over the image instead of on its own
   ground. The glyph is inked in the attribute's foreground with a one pixel
   halo in its background, and the picture shows everywhere else. Marks last
   until the next ls_tui_image call, and a mark is ignored once the cell has
   been written with anything else, so a panel drawn over the map is solid.
   Outside the image rectangle a marked cell draws normally. */
void ls_tui_glass(int col, int row);
void ls_tui_put_glass(tui_surface *sf, tui_rect clip, int x, int y, int16_t ch, uint8_t attr);

/* For the screen recorder: the image as it was last handed over (false when
   there is none), and the glass marks as last presented, one bit per cell. */
bool ls_tui_image_now(tui_rect *cells, const uint16_t **src, int *w, int *h, uint32_t *serial);
const uint8_t *ls_tui_glass_now(size_t *bytes);

#define LS_TUI_TRACE(eighths) ((char)(0xA0 + (((eighths) < 1 ? 1 :                                               (eighths) > 8 ? 8 : (eighths)) - 1)))

/* Pick the face. The grid size follows from it, so this takes effect
   at the next begin(); call it before. Screens never see the cell size. */
void ls_tui_set_font(const ls_font_t *font);
/* Threshold glyph coverage at native pixels; useful for small outdoor text. */
void ls_tui_set_crisp_text(bool on);
bool ls_tui_crisp_text(void);

int ls_tui_font_count(void);

/* Which of them is wanted, and which one a running session started
   with. Setting it does not change a running session - the grid is fixed at
   ls_tui_begin - so ls_tui_begin reads it and the caller restarts. */
int  ls_tui_font_index(void);
void ls_tui_set_font_index(int index);
const ls_font_t *ls_tui_font_at(int index);
const char *ls_tui_font_label(int index);
const ls_font_t *ls_tui_font(void);

/* A camera hole in the glass, native portrait pixels. Zero radius: none.
   The grid keeps its full size; chrome lays itself out around the hole. */
void ls_tui_set_cutout(int cx, int cy, int radius);
/* The cells the hole covers in the current grid and rotation. False, and a
   zero rect, when there is no hole or it misses the grid. */
bool ls_tui_cutout(tui_rect *cells);
/* Landscape handedness, default clockwise. Apply before begin for safe insets. */
void ls_tui_set_rotation_cw(bool clockwise);
/* Told at every begin which board edge the content's top now faces, in
   screen axes (x right, y top): (0,1) portrait, (-1,0) clockwise landscape,
   (1,0) counter-clockwise. Instruments that point somewhere (the compass)
   use it so their heading matches the picture. */
void ls_tui_set_orientation_listener(void (*fn)(int ux, int uy));

/* The panel's corner radius in pixels, which is how far the grid has
   to stand off its corners and NOT how far it has to stand off its edges.
   Zero for a square panel. Takes effect on the next begin(). */
void ls_tui_set_corner_radius(int px);
int  ls_tui_corner_radius(void);

int  ls_tui_corner_pad(int row);

/* Convert a touch controller pixel - native panel orientation - into
   a grid cell. False when the pixel is outside the grid, including the inset
   margin, so a tap on a rounded corner is correctly nothing. */
bool ls_tui_pixel_to_cell(int native_x, int native_y, int *col, int *row);
/* The same pixel in logical pixels from the cell grid's top-left corner,
   cell (c, r) spanning c*cell_w and r*cell_h for every row but the bars.
   Not clipped: a finger dragged off the grid still has a position. */
bool ls_tui_pixel_to_grid(int native_x, int native_y, int *gx, int *gy);

/* Microseconds spent inside the last present, and cells drawn. */
void ls_tui_last_cost(uint32_t *us, int *cells);
/* Last present: image destination bytes (including moves), requested native
   row writeback bytes. Excludes cache-line rounding and panel scan traffic. */
void ls_tui_last_traffic(uint32_t *image_bytes, uint32_t *flush_bytes);
/* Slowest frame since the last call, then reset (console `display`). */
uint32_t ls_tui_max_cost_take(void);
/* The costliest present since the last call (which resets it): total time,
   cells drawn, the cache writeback's share, and coverage ramps rebuilt. */
void ls_tui_peak_cost(uint32_t *us, int *cells, uint32_t *sync_us, uint32_t *ramps);

void ls_tui_dump(void);
void ls_tui_remote_geometry(void);

/* ---------------------------------------------------- borrowed pixels -- */

/* A rectangle of the grid whose pixels somebody else owns. */

/* Snapshot the clean basemap. Cells replaced by overlays retain the TUI theme.
 * NULL clears it on leave. Palette changes invalidate only basemap cells. */
void ls_tui_basemap(tui_surface *sf,tui_rect cells,const uint16_t palette[16]);

void ls_tui_reserve(tui_rect cells);
tui_rect ls_tui_reserved(void);

/* Push an image into a reserved rectangle. */

void ls_tui_blit_rgb565(tui_rect cells, const uint16_t *src,
                        int src_w, int src_h);

#ifdef __cplusplus
}
#endif

#endif /* LS_TUI_H */
