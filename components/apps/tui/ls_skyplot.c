/* See ls_skyplot.h. Pure: no surface, no state, no receiver. */
#include "ls_skyplot.h"

#include <math.h>

bool ls_sky_project(int az_deg, int el_deg, int rows, int cols,
                    int cell_w, int cell_h, int *row, int *col)
{
    if (!row || !col || rows < 3 || cols < 3) return false;
    if (cell_w <= 0 || cell_h <= 0) return false;
    /* An empty GSV slot is all zeros. Letting it through would stack every
       unfilled satellite on the horizon due north, which reads as a cluster
       of real satellites in one direction - the exact picture this plot
       exists to make meaningful. */
    if (el_deg < 0 || el_deg > 90) return false;

    while (az_deg < 0)    az_deg += 360;
    while (az_deg >= 360) az_deg -= 360;

    const int cy = rows / 2;
    const int cx = cols / 2;

    const double r_rows = (double)cy;
    double r_cols = r_rows * ((double)cell_h / (double)cell_w);
    if (r_cols > (double)cx) r_cols = (double)cx;

    /* Elevation is the radius INVERTED: 90 is straight up and belongs in the
       middle, 0 is the horizon and belongs on the rim. */
    const double k = (double)(90 - el_deg) / 90.0;

    /* North up, east right. Screen rows increase downwards, so north - which
       is up - is a NEGATIVE row offset, and that minus sign is the whole of
       what makes this not mirrored. */
    const double a = (double)az_deg * (M_PI / 180.0);
    const double dy = -cos(a) * k * r_rows;
    const double dx =  sin(a) * k * r_cols;

    int r = cy + (int)lround(dy);
    int c = cx + (int)lround(dx);

    if (r < 0) r = 0;
    if (r >= rows) r = rows - 1;
    if (c < 0) c = 0;
    if (c >= cols) c = cols - 1;

    *row = r;
    *col = c;
    return true;
}

bool ls_sky_located(int az_deg, int el_deg)
{
    return !(az_deg == 0 && el_deg == 0);
}

char ls_sky_glyph(int snr_db)
{
    /* Seen but not tracked. */

    if (snr_db <= 0)  return 'o';
    if (snr_db < 20)  return '-';
    if (snr_db < 30)  return '+';
    if (snr_db < 40)  return '*';
    return '#';
}
