#include "ui/ls_spectrum_waterfall_logic.h"

#include <stdint.h>
#include <string.h>

static int clamp_int(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

bool ls_spectrum_layout(int width, int height, int split_pct, bool fullscreen,
                        size_t bytes_per_pixel, ls_spectrum_layout_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (width < 2 || height < 2 || bytes_per_pixel == 0 ||
        width > LS_SPECTRUM_CANVAS_MAX_WIDTH ||
        height > LS_SPECTRUM_CANVAS_MAX_HEIGHT)
        return false;

    size_t pixels = (size_t)width * (size_t)height;
    if (pixels > SIZE_MAX / bytes_per_pixel) return false;

    split_pct = clamp_int(split_pct, 0, 100);
    out->width = width;
    out->allocation_height = height;
    out->allocation_bytes = pixels * bytes_per_pixel;

    if (fullscreen || split_pct == 0) {
        out->waterfall_height = height;
        out->waterfall_visible = true;
        return true;
    }
    if (split_pct == 100) {
        out->spectrum_height = height;
        out->spectrum_visible = true;
        return true;
    }

    out->spectrum_height = height * split_pct / 100;
    if (out->spectrum_height < 1) out->spectrum_height = 1;
    if (out->spectrum_height >= height) out->spectrum_height = height - 1;
    out->waterfall_height = height - out->spectrum_height;
    out->spectrum_visible = true;
    out->waterfall_visible = true;
    return true;
}

bool ls_spectrum_column_bins(int column, int columns, int bins,
                             int *first, int *past_last)
{
    if (!first || !past_last || column < 0 || columns < 1 || bins < 1 ||
        column >= columns)
        return false;
    int lo = (int)(((int64_t)column * bins) / columns);
    int hi = (int)(((int64_t)(column + 1) * bins) / columns);
    if (hi <= lo) hi = lo + 1;
    if (hi > bins) hi = bins;
    *first = lo;
    *past_last = hi;
    return lo >= 0 && lo < hi && hi <= bins;
}

bool ls_spectrum_coord_bin(int x, int width, int bins, int *bin)
{
    if (!bin || width < 1 || bins < 1 || x < 0) return false;
    if (x >= width) x = width - 1;
    int value = (int)(((int64_t)x * bins) / width);
    if (value >= bins) value = bins - 1;
    *bin = value;
    return true;
}

static int step_ladder(const int *steps, int count, int value, int direction)
{
    int nearest = 0;
    int distance = value > steps[0] ? value - steps[0] : steps[0] - value;
    for (int i = 1; i < count; ++i) {
        int d = value > steps[i] ? value - steps[i] : steps[i] - value;
        if (d < distance) { nearest = i; distance = d; }
    }
    if (direction < 0 && nearest > 0) --nearest;
    if (direction > 0 && nearest + 1 < count) ++nearest;
    return steps[nearest];
}

int ls_spectrum_split_step(int split_pct, int direction)
{
    static const int steps[] = { 0, 25, 50, 75, 100 };
    return step_ladder(steps, 5, clamp_int(split_pct, 0, 100), direction);
}

int ls_spectrum_contrast_step(int contrast_pct, int direction)
{
    static const int steps[] = { 50, 75, 100, 150, 200 };
    return step_ladder(steps, 5, clamp_int(contrast_pct, 50, 200), direction);
}

bool ls_spectrum_buffer_resize(ls_spectrum_buffer_state_t *state,
                               int width, int height,
                               size_t bytes_per_pixel)
{
    if (!state) return false;
    ls_spectrum_layout_t layout;
    if (!ls_spectrum_layout(width, height, 0, false, bytes_per_pixel, &layout))
        return false;
    state->width = width;
    state->height = height;
    state->allocation_bytes = layout.allocation_bytes;
    state->allocated = true;
    state->has_data = false;
    return true;
}

void ls_spectrum_buffer_data(ls_spectrum_buffer_state_t *state, bool has_data)
{
    if (state && state->allocated) state->has_data = has_data;
}

void ls_spectrum_buffer_forget(ls_spectrum_buffer_state_t *state)
{
    if (state) memset(state, 0, sizeof(*state));
}
