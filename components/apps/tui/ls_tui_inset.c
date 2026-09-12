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

int ls_tui_corner_cells(int screen_w, int screen_h, int cell_w, int cell_h,
                        int radius, int ox, int oy, int row)
{
    if (screen_w <= 0 || screen_h <= 0 || cell_w <= 0 || cell_h <= 0) return 0;
    if (ox < 0 || oy < 0 || row < 0) return 0;

    /* The grid exactly as ls_tui_begin sizes it from the same inset. */
    const int cols = (screen_w - 2 * ox) / cell_w;
    const int rows = (screen_h - 2 * oy) / cell_h;
    if (cols <= 0 || row >= rows) return 0;

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
