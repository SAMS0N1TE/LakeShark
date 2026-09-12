

#ifndef LS_SKYPLOT_H
#define LS_SKYPLOT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Project one satellite onto a plot `rows` tall inside a rect
   `cols` wide, with cells `cell_w` by `cell_h` pixels.

   Writes the cell offsets from the rect's top-left into `*row` and `*col`.
   Returns false for an elevation outside 0..90, which a receiver does report
   - an empty GSV slot is all zeros and would otherwise pile every unfilled
   satellite onto the horizon due north. */
bool ls_sky_project(int az_deg, int el_deg, int rows, int cols,
                    int cell_w, int cell_h, int *row, int *col);

/* Has the receiver actually LOCATED this satellite? */

bool ls_sky_located(int az_deg, int el_deg);

char ls_sky_glyph(int snr_db);

#ifdef __cplusplus
}
#endif

#endif /* LS_SKYPLOT_H */
