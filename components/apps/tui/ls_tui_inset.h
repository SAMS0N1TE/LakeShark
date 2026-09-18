/* How far in the grid has to start so a rounded panel corner cannot clip it. */

#ifndef LS_TUI_INSET_H
#define LS_TUI_INSET_H

#ifdef __cplusplus
extern "C" {
#endif

/* True when a grid starting at (ox, oy) clears a corner of `radius` pixels. */
int ls_tui_corner_clear(int ox, int oy, int radius);

/* Where a centred grid starts, for this panel and this cell size.

   "Best" is the smallest WORST margin, not the most cells: the two disagree,
   and the one the eye judges is the margin. See the note in the .c. Both
   outputs are written even when the radius is zero, so a caller need not
   check. */
void ls_tui_corner_inset(int screen_w, int screen_h,
                         int cell_w, int cell_h, int radius,
                         int *ox, int *oy);

/* The first pixel column drawable on scanline `y` of a panel `screen_h` tall
   with corners of `radius`.

   The mirror of corner_cells and for the other half of the same job: the grid
   keeps its TEXT out of the corners, and the chrome behind that text has to
   run past them to the glass or the bar reads as floating inside a black
   frame. Zero everywhere a corner does not reach, `radius` on the very first
   and last scanline, and symmetric - the right edge is screen_w minus this. */
int ls_tui_row_inset(int y, int screen_h, int radius);

/* How many cells at each end of grid row `row` sit closer to a rounded corner than the grid sits to the panel's straight edges - the cells a word should not be drawn in. */

int ls_tui_corner_cells(int screen_w, int screen_h, int cell_w, int cell_h,
                        int radius, int ox, int oy, int row);

#ifdef __cplusplus
}
#endif

#endif /* LS_TUI_INSET_H */
