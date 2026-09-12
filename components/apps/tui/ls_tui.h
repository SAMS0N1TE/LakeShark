

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

/* Force the next present to redraw every cell. */
void ls_tui_invalidate(void);

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
#define LS_TUI_SHADE_25     ((char)0x90)
#define LS_TUI_SHADE_50     ((char)0x91)
#define LS_TUI_SHADE_75     ((char)0x92)
#define LS_TUI_SHADE_FULL   ((char)0x93)

#define LS_TUI_TRACE(eighths) ((char)(0xA0 + (((eighths) < 1 ? 1 :                                               (eighths) > 8 ? 8 : (eighths)) - 1)))

/* Pick the face. The grid size follows from it, so this takes effect
   at the next begin(); call it before. Screens never see the cell size. */
void ls_tui_set_font(const ls_font_t *font);

int ls_tui_font_count(void);

/* Which of them is wanted, and which one a running session started
   with. Setting it does not change a running session - the grid is fixed at
   ls_tui_begin - so ls_tui_begin reads it and the caller restarts. */
int  ls_tui_font_index(void);
void ls_tui_set_font_index(int index);
const ls_font_t *ls_tui_font_at(int index);
const char *ls_tui_font_label(int index);
const ls_font_t *ls_tui_font(void);

/* Landscape writes transposed; if the image comes out inverted the panel's
   handedness is the other way. Default true, toggle to find out. */
void ls_tui_set_rotation_cw(bool clockwise);

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

/* Microseconds spent inside the last present, and cells drawn. */
void ls_tui_last_cost(uint32_t *us, int *cells);

void ls_tui_dump(void);

/* ---------------------------------------------------- borrowed pixels -- */

/* A rectangle of the grid whose pixels somebody else owns. */

void ls_tui_reserve(tui_rect cells);
tui_rect ls_tui_reserved(void);

/* Push an image into a reserved rectangle. */

void ls_tui_blit_rgb565(tui_rect cells, const uint16_t *src,
                        int src_w, int src_h);

#ifdef __cplusplus
}
#endif

#endif /* LS_TUI_H */
