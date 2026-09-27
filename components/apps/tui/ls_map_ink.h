/* Drawing over the map: a dot canvas at sextant resolution, two dots across
   and three down per cell, flushed as glass cells so the picture shows
   between the strokes. Lines, rings and symbols go in as dots; text goes
   straight to the surface afterwards. UI task only. */

#ifndef LS_MAP_INK_H
#define LS_MAP_INK_H

#include <stdbool.h>
#include <stdint.h>

#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start a canvas over `cells`. False when the rectangle is empty or larger
   than the canvas can hold; every other call is then a no-op. */
bool ls_ink_begin(tui_rect cells);

/* In dots from the rectangle's top left: 0..2w-1 across, 0..3h-1 down.
   A cell takes the attribute of the highest priority dot written into it,
   the later one on a tie. */
void ls_ink_dot(int x, int y, uint8_t attr, uint8_t prio);

/* `dash` 0 is solid; n draws n dots and skips n; -n draws one in every n. */
void ls_ink_line(int x0, int y0, int x1, int y1, uint8_t attr, uint8_t prio, int dash);

/* An ellipse with radii in dots on each axis, so a ring measured in map
   pixels stays round whatever the cell shape. */
void ls_ink_ellipse(int cx, int cy, int rx, int ry, uint8_t attr, uint8_t prio, int dash);

/* Whether the canvas already has a dot in this cell. */
bool ls_ink_cell_used(int col, int row);

/* Write every cell with a dot in it, as glass. `skip` cells are left alone
   (a panel drawn over the map). */
void ls_ink_flush(tui_surface *sf, tui_rect skip);

/* One colour for everything drawn over the map: every hue becomes `base`,
   bright staying bright and dim staying dim, so a red night map keeps its
   aircraft and labels red. 0 turns it off. */
void    ls_ink_set_tint(uint8_t base);
uint8_t ls_ink_tinted(uint8_t attr);

/* Text over the map: glass characters, spaces left see-through. */
void ls_ink_text(tui_surface *sf, tui_rect clip, int x, int y, const char *s, int n, uint8_t attr);

#ifdef __cplusplus
}
#endif

#endif /* LS_MAP_INK_H */
