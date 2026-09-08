#ifndef LS_SPECTRUM_WATERFALL_LOGIC_H
#define LS_SPECTRUM_WATERFALL_LOGIC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LS-703: all supported panels fit inside this canvas ceiling (720 x 400).
 * Refuse larger requests instead of letting a corrupt measurement consume an
 * unbounded part of PSRAM.  The renderer allocates exactly width * height and
 * never places this storage in internal RAM. */
#define LS_SPECTRUM_CANVAS_MAX_WIDTH   720
#define LS_SPECTRUM_CANVAS_MAX_HEIGHT  400

typedef struct {
    int width;
    int spectrum_height;
    int waterfall_height;
    int allocation_height;
    size_t allocation_bytes;
    bool spectrum_visible;
    bool waterfall_visible;
} ls_spectrum_layout_t;

typedef struct {
    int width;
    int height;
    size_t allocation_bytes;
    bool allocated;
    bool has_data;
} ls_spectrum_buffer_state_t;

/* Split is the percentage assigned to the spectrum. Fullscreen is a
 * waterfall-only presentation and does not destroy the remembered split. */
bool ls_spectrum_layout(int width, int height, int split_pct, bool fullscreen,
                        size_t bytes_per_pixel, ls_spectrum_layout_t *out);

/* Map a destination column to a non-empty, in-range source interval. */
bool ls_spectrum_column_bins(int column, int columns, int bins,
                             int *first, int *past_last);

/* Convert a plot coordinate to a bin. The right and bottom edges clamp to the
 * last valid coordinate; negative or empty geometry is rejected. */
bool ls_spectrum_coord_bin(int x, int width, int bins, int *bin);

/* Shared five-stop interaction used by REC, FM and P25. */
int ls_spectrum_split_step(int split_pct, int direction);
int ls_spectrum_contrast_step(int contrast_pct, int direction);

/* Allocation-independent lifecycle used by the PSRAM wrapper. Invalid resize
 * requests preserve the last usable state; a valid resize starts empty. */
bool ls_spectrum_buffer_resize(ls_spectrum_buffer_state_t *state,
                               int width, int height,
                               size_t bytes_per_pixel);
void ls_spectrum_buffer_data(ls_spectrum_buffer_state_t *state, bool has_data);
void ls_spectrum_buffer_forget(ls_spectrum_buffer_state_t *state);

#ifdef __cplusplus
}
#endif

#endif
