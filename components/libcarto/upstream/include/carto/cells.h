#ifndef CARTO_CELLS_H
#define CARTO_CELLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Terminal-cell reduction.
 *
 * Turns a subcell RGB grid into one glyph and one or two colours per terminal
 * cell -- the step the Python renderer spent most of a frame on. The caller
 * supplies the grid already scaled to cols*cell_w by rows*cell_h; everything
 * after that (luminance, local contrast stretch, glyph selection, cell colour)
 * happens here.
 */

typedef enum {
    CARTO_CELL_ASCII    = 0,  /* 1x1 subcells, glyph straight from the ramp */
    CARTO_CELL_QUADRANT = 1,  /* 2x2 subcells */
    CARTO_CELL_BRAILLE  = 2,  /* 2x4 subcells */
    CARTO_CELL_HALF     = 3   /* 1x2 subcells, upper half block, fg + bg */
} carto_cell_mode;

typedef enum {
    CARTO_THRESH_ADAPTIVE = 0,  /* per-tile stretch, bilinearly blended */
    CARTO_THRESH_GLOBAL   = 1,  /* one stretch over the whole grid */
    CARTO_THRESH_FIXED    = 2   /* no stretch, just clamp */
} carto_thresh_mode;

typedef enum {
    CARTO_ORIENT_DARK   = 0,  /* light ink on a dark ground */
    CARTO_ORIENT_BRIGHT = 1,  /* dark ink on a light ground */
    CARTO_ORIENT_GUESS  = 2   /* decide from the frame mean */
} carto_orientation;

typedef struct {
    int32_t         mode;            /* carto_cell_mode */
    int32_t         cols;
    int32_t         rows;
    int32_t         mono;            /* 1 = drive colour from luminance only */
    int32_t         want_color;      /* 0 = skip the colour pass entirely */
    int32_t         orientation;     /* carto_orientation */
    int32_t         threshold_mode;  /* carto_thresh_mode */
    float           black_pct;
    float           white_pct;
    int32_t         tile_grid;
    float           signal_floor;
    float           signal_gamma;
    int32_t         shaded;
    int32_t         palette_len;
    const uint32_t *palette;         /* codepoints, darkest first */
} carto_cell_opts;

/* `rgb` is cols*cell_w by rows*cell_h, 3 bytes per pixel, tightly packed.
 * `glyph` receives cols*rows codepoints. `fg` (and `bg`, half mode only)
 * receive cols*rows colours packed 0x00RRGGBB; either may be NULL when
 * want_color is 0.
 *
 * Returns 0, or -1 on a bad argument.
 */
int carto_cellify(const uint8_t *rgb, int32_t w, int32_t h,
                  const carto_cell_opts *opts,
                  uint32_t *glyph, uint32_t *fg, uint32_t *bg);

/* As above, but straight from libcarto's own RGB565 framebuffer. `lut` is a
 * 65536-entry little-endian RGBA table (red in the low byte) with the tone
 * chain already folded in, and
 * the downsample to the subcell grid happens here -- so no full-resolution RGB
 * image is materialised and resampled outside.
 *
 * Returns -1 when the target is larger than the source, which wants a
 * different filter; the caller should fall back.
 */
int carto_cellify_rgb565(const uint16_t *src, int32_t sw, int32_t sh,
                         const uint32_t *lut, const carto_cell_opts *opts,
                         uint32_t *glyph, uint32_t *fg, uint32_t *bg);

/* Colour histogram over a 16-bit index buffer; `bins` must hold 65536 entries
 * and is overwritten. Used to find the frame's mean luminance and the small set
 * of colours it actually contains. */
void carto_histogram_u16(const uint16_t *src, int64_t n, int64_t *bins);

/* Subcell size for a mode, so callers can scale the grid to match. */
void carto_cell_geometry(int32_t mode, int32_t *cell_w, int32_t *cell_h);

#ifdef __cplusplus
}
#endif

#endif
