/* See ls_tui_inset.h. Integer arithmetic only: this runs at grid
   setup on a board with no FPU worth using and the exact comparison is the
   whole point. */
#include "ls_tui_inset.h"

int ls_tui_corner_clear(int ox, int oy, int radius)
{
    if (radius <= 0) return 1;
    if (ox < 0 || oy < 0) return 0;
    /* Past the arc's centre on either axis is past the arc. */
    if (ox >= radius || oy >= radius) return 1;
    const int dx = radius - ox, dy = radius - oy;
    return dx * dx + dy * dy <= radius * radius;
}

void ls_tui_corner_inset(int screen_w, int screen_h,
                         int cell_w, int cell_h, int radius,
                         int *ox, int *oy)
{
    if (ox) *ox = 0;
    if (oy) *oy = 0;
    if (cell_w <= 0 || cell_h <= 0 || screen_w <= 0 || screen_h <= 0) return;

    int best_x = -1, best_y = -1, best_worst = 0;
    long best_cells = -1;

    for (int cols = screen_w / cell_w; cols >= 1; cols--) {
        const int x = (screen_w - cols * cell_w) / 2;
        for (int rows = screen_h / cell_h; rows >= 1; rows--) {
            const int y = (screen_h - rows * cell_h) / 2;
            if (!ls_tui_corner_clear(x, y, radius)) continue;

            const int worst = x > y ? x : y;
            const long cells = (long)cols * rows;
            if (best_x < 0 || worst < best_worst ||
                (worst == best_worst && cells > best_cells)) {
                best_worst = worst;
                best_cells = cells;
                best_x = x;
                best_y = y;
            }
            /* Fewer rows only ever means a bigger vertical margin, so this
               column is settled the moment one fits. */
            break;
        }
    }
    if (best_x < 0) return;
    if (ox) *ox = best_x;
    if (oy) *oy = best_y;
}

/* See ls_tui_inset.h. */

int ls_tui_row_inset(int y, int screen_h, int radius)
{
    if (radius <= 0 || screen_h <= 0) return 0;
    if (y < 0 || y >= screen_h) return radius;

    /* Which corner this scanline is near. The far half of the panel is the
       near half measured from the other end, so one arc answers for both. */
    const int below = screen_h - 1 - y;
    const int gap = y < below ? y : below;
    if (gap >= radius) return 0;

    /* Walk out until the arc lets go. The same stepping loop the grid inset
       uses, kept rather than a square root: this runs per scanline on a core
       with nothing to spare for the FPU, and the exact comparison is what
       makes the two agree about where the arc is. */
    int inset = 0;
    while (inset < radius && !ls_tui_corner_clear(inset, gap, radius)) inset++;
    return inset;
}

/* See ls_tui_inset.h. */

int ls_tui_corner_cells(int screen_w, int screen_h, int cell_w, int cell_h,
                        int radius, int ox, int oy, int row)
{
    return ls_tui_grid_corner_cells(screen_w, screen_h, cell_w, cell_h,
                                    radius, ox, oy,
                                    cell_w > 0 ? (screen_w - 2 * ox) / cell_w : 0,
                                    cell_h > 0 ? (screen_h - 2 * oy) / cell_h : 0,
                                    row);
}

int ls_tui_grid_corner_cells(int screen_w, int screen_h, int cell_w, int cell_h,
                             int radius, int ox, int oy, int cols, int rows,
                             int row)
{
    if (screen_w <= 0 || screen_h <= 0 || cell_w <= 0 || cell_h <= 0) return 0;
    if (ox < 0 || oy < 0 || row < 0 || cols <= 0 || row >= rows) return 0;

    /* The margin the grid keeps from the straight edges: the least of the
       four, since the far two carry the leftover pixel. */
    const int right  = screen_w - ox - cols * cell_w;
    const int bottom = screen_h - oy - rows * cell_h;
    int m = ox;
    if (oy < m)     m = oy;
    if (right < m)  m = right;
    if (bottom < m) m = bottom;

    /* An arc no larger than the margin lies inside it, so every cell has the
       standoff already. That is also every square panel. */
    if (radius <= m) return 0;

    const int above = oy + row * cell_h;
    const int below = screen_h - (oy + (row + 1) * cell_h);
    const int gap_y = above < below ? above : below;

    int n = 0;
    while (n < cols / 2) {
        const int left_gap  = ox + n * cell_w;
        const int right_gap = screen_w - (ox + (cols - n) * cell_w);
        const int gap_x = left_gap < right_gap ? left_gap : right_gap;
        if (ls_tui_corner_clear(gap_x - m, gap_y - m, radius - m)) break;
        n++;
    }
    return n;
}

void ls_tui_native_to_logical(int nx, int ny, int rotation,
                              int screen_w, int screen_h, int *lx, int *ly)
{
    /* The inverse of the blitter's store: clockwise puts logical x at native
       row screen_w - 1 - x, counter-clockwise at native row x. */
    int x = nx, y = ny;
    if (rotation > 0)      { x = screen_w - 1 - ny; y = nx; }
    else if (rotation < 0) { x = ny; y = screen_h - 1 - nx; }
    if (lx) *lx = x;
    if (ly) *ly = y;
}

/* Squared distance from c to the nearest pixel of [a0, a1). */
static long nearest_sq(int c, int a0, int a1)
{
    const long d = c < a0 ? a0 - c : c >= a1 ? c - (a1 - 1) : 0;
    return d * d;
}

int ls_tui_cutout_cells(int cx, int cy, int r, int ox, int oy,
                        int cell_w, int cell_h, int cols, int rows,
                        int top_shift, int bottom_shift,
                        int *x, int *y, int *w, int *h)
{
    int x0 = cols, y0 = rows, x1 = -1, y1 = -1;
    if (r > 0 && cell_w > 0 && cell_h > 0) {
        for (int row = 0; row < rows; row++) {
            int top = oy + row * cell_h;
            if (row == 0) top += top_shift;
            else if (row == rows - 1) top += bottom_shift;
            const long dy = nearest_sq(cy, top, top + cell_h);
            if (dy > (long)r * r) continue;
            for (int c = 0; c < cols; c++) {
                const int left = ox + c * cell_w;
                if (dy + nearest_sq(cx, left, left + cell_w) > (long)r * r)
                    continue;
                if (c < x0) x0 = c;
                if (c > x1) x1 = c;
                if (row < y0) y0 = row;
                if (row > y1) y1 = row;
            }
        }
    }
    const int hit = x1 >= 0;
    if (x) *x = hit ? x0 : 0;
    if (y) *y = hit ? y0 : 0;
    if (w) *w = hit ? x1 - x0 + 1 : 0;
    if (h) *h = hit ? y1 - y0 + 1 : 0;
    return hit;
}
