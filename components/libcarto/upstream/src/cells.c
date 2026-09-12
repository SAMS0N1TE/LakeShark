#include "carto/cells.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Mirrors cartotui/rendering/threshold.py and the backends in renderer.py.
 * The arithmetic is deliberately written to match numpy's: float32 for the
 * signal, float64 where numpy widens (percentiles, the braille cell mean),
 * rint for np.round's round-half-to-even, and truncation where numpy casts. */

#define CARTO_BRAILLE_BASE 0x2800u

static const uint32_t CARTO_QUAD_GLYPHS[16] = {
    0x0020, 0x2597, 0x2596, 0x2584,
    0x259D, 0x2590, 0x259E, 0x259F,
    0x2598, 0x259A, 0x258C, 0x2599,
    0x2580, 0x259C, 0x259B, 0x2588
};

/* Bit per subcell, row-major over the 4x2 braille grid. */
static const uint8_t CARTO_BRAILLE_BITS[4][2] = {
    {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}
};

void carto_cell_geometry(int32_t mode, int32_t *cell_w, int32_t *cell_h) {
    int32_t w = 1, h = 1;
    switch (mode) {
        case CARTO_CELL_QUADRANT: w = 2; h = 2; break;
        case CARTO_CELL_BRAILLE:  w = 2; h = 4; break;
        case CARTO_CELL_HALF:     w = 1; h = 2; break;
        default:                  w = 1; h = 1; break;
    }
    if (cell_w) *cell_w = w;
    if (cell_h) *cell_h = h;
}

/* ---------------------------------------------------------------- percentile */

/* Order statistic by quickselect, matching numpy's linear interpolation:
 * idx = p/100 * (n-1), then a[floor] + frac * (a[floor+1] - a[floor]).
 * numpy partitions rather than sorts, and so does this -- sorting 128k floats
 * a frame would cost more than the rest of the pass put together. */
static float select_kth(float *a, int32_t n, int32_t k) {
    int32_t lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = a[(lo + hi) >> 1];
        int32_t i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) {
                float t = a[i]; a[i] = a[j]; a[j] = t;
                i++; j--;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else break;
    }
    return a[k];
}

/* Smallest value strictly after position k, i.e. a[k+1] once a[k] is in place. */
static float min_above(const float *a, int32_t n, int32_t k) {
    float best = a[k + 1];
    for (int32_t i = k + 2; i < n; ++i)
        if (a[i] < best) best = a[i];
    return best;
}

static float percentile_into(float *scratch, int32_t n, double pct) {
    if (n <= 0) return 0.0f;
    if (n == 1) return scratch[0];
    double pos = (pct / 100.0) * (double)(n - 1);
    if (pos <= 0.0) {
        float lo = scratch[0];
        for (int32_t i = 1; i < n; ++i) if (scratch[i] < lo) lo = scratch[i];
        return lo;
    }
    if (pos >= (double)(n - 1)) {
        float hi = scratch[0];
        for (int32_t i = 1; i < n; ++i) if (scratch[i] > hi) hi = scratch[i];
        return hi;
    }
    int32_t base = (int32_t)floor(pos);
    double frac = pos - (double)base;
    float lo = select_kth(scratch, n, base);
    if (frac == 0.0) return lo;
    float hi = min_above(scratch, n, base);
    return (float)((double)lo + frac * ((double)hi - (double)lo));
}

/* ------------------------------------------------------------------- signal */

static void luminance(const uint8_t *rgb, int32_t n, float *out) {
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *p = rgb + (size_t)i * 3;
        out[i] = (float)((0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0);
    }
}

static void global_stretch(float *sig, int32_t n, float *scratch,
                           double black_pct, double white_pct) {
    if (black_pct <= 0.0 && white_pct >= 100.0) {
        for (int32_t i = 0; i < n; ++i) {
            float v = sig[i];
            sig[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
        return;
    }
    memcpy(scratch, sig, (size_t)n * sizeof(float));
    float lo = percentile_into(scratch, n, black_pct);
    memcpy(scratch, sig, (size_t)n * sizeof(float));
    float hi = percentile_into(scratch, n, white_pct);
    if (hi - lo < 1e-3f) {
        for (int32_t i = 0; i < n; ++i) {
            float v = sig[i];
            sig[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
        return;
    }
    float inv = 1.0f / (hi - lo);
    for (int32_t i = 0; i < n; ++i) {
        float v = (sig[i] - lo) * inv;
        sig[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
}

/* CLAHE-style local stretch: percentiles per tile, then the four surrounding
 * tile mappings blended bilinearly per pixel. */
static int adaptive_stretch(float *sig, int32_t w, int32_t h, float *scratch,
                            int32_t grid, double black_pct, double white_pct,
                            float uniform_floor) {
    int32_t g = grid < 2 ? 2 : grid;
    int32_t *ys = (int32_t *)malloc((size_t)(g + 1) * sizeof(int32_t));
    int32_t *xs = (int32_t *)malloc((size_t)(g + 1) * sizeof(int32_t));
    float *tile_lo = (float *)malloc((size_t)g * g * sizeof(float));
    float *tile_inv = (float *)malloc((size_t)g * g * sizeof(float));
    uint8_t *tile_uni = (uint8_t *)malloc((size_t)g * g);
    if (!ys || !xs || !tile_lo || !tile_inv || !tile_uni) {
        free(ys); free(xs); free(tile_lo); free(tile_inv); free(tile_uni);
        return -1;
    }

    /* np.linspace(0, H, g+1, dtype=int) truncates toward zero. */
    for (int32_t i = 0; i <= g; ++i) {
        ys[i] = (int32_t)((double)h * (double)i / (double)g);
        xs[i] = (int32_t)((double)w * (double)i / (double)g);
    }

    for (int32_t ti = 0; ti < g; ++ti) {
        for (int32_t tj = 0; tj < g; ++tj) {
            int32_t y0 = ys[ti], y1 = ys[ti + 1], x0 = xs[tj], x1 = xs[tj + 1];
            int32_t bw = x1 - x0, bh = y1 - y0;
            int32_t idx = ti * g + tj;
            if (bw <= 0 || bh <= 0) {
                tile_lo[idx] = 0.0f; tile_inv[idx] = 1.0f; tile_uni[idx] = 0;
                continue;
            }
            /* One copy serves both percentiles: quickselect only permutes,
             * so the second selection still sees the same multiset. */
            int32_t count = 0;
            for (int32_t y = y0; y < y1; ++y)
                for (int32_t x = x0; x < x1; ++x)
                    scratch[count++] = sig[(size_t)y * w + x];
            float lo = percentile_into(scratch, count, black_pct);
            float hi = percentile_into(scratch, count, white_pct);
            if ((hi - lo) < uniform_floor) {
                tile_uni[idx] = 1; tile_lo[idx] = 0.0f; tile_inv[idx] = 1.0f;
            } else {
                float spread = hi - lo;
                if (spread < 1e-3f) spread = 1e-3f;
                tile_uni[idx] = 0; tile_lo[idx] = lo; tile_inv[idx] = 1.0f / spread;
            }
        }
    }

    /* Tile centres, and each pixel's position between the two that bracket it.
     * The column table is built once rather than searched per pixel. */
    int32_t *ixs = (int32_t *)malloc((size_t)w * sizeof(int32_t));
    float *wxs = (float *)malloc((size_t)w * sizeof(float));
    if (!ixs || !wxs) {
        free(ixs); free(wxs);
        free(ys); free(xs); free(tile_lo); free(tile_inv); free(tile_uni);
        return -1;
    }
    for (int32_t x = 0; x < w; ++x) {
        double cxv = (double)x;
        int32_t ix = 0;
        for (int32_t t = 0; t < g; ++t) {
            double c = ((double)xs[t] + (double)xs[t + 1]) * 0.5;
            if (c <= cxv) ix = t; else break;
        }
        if (ix > g - 2) ix = g - 2;
        if (ix < 0) ix = 0;
        double cx0 = ((double)xs[ix] + (double)xs[ix + 1]) * 0.5;
        double cx1 = ((double)xs[ix + 1] + (double)xs[ix + 2]) * 0.5;
        double dx = cx1 - cx0; if (dx < 1e-6) dx = 1e-6;
        float wx = (float)((cxv - cx0) / dx);
        ixs[x] = ix;
        wxs[x] = wx < 0.0f ? 0.0f : (wx > 1.0f ? 1.0f : wx);
    }

    for (int32_t y = 0; y < h; ++y) {
        double cyv = (double)y;
        int32_t iy = 0;
        for (int32_t t = 0; t < g; ++t) {
            double c = ((double)ys[t] + (double)ys[t + 1]) * 0.5;
            if (c <= cyv) iy = t; else break;
        }
        if (iy > g - 2) iy = g - 2;
        if (iy < 0) iy = 0;
        double cy0 = ((double)ys[iy] + (double)ys[iy + 1]) * 0.5;
        double cy1 = ((double)ys[iy + 1] + (double)ys[iy + 2]) * 0.5;
        double dy = cy1 - cy0; if (dy < 1e-6) dy = 1e-6;
        float wy = (float)((cyv - cy0) / dy);
        if (wy < 0.0f) wy = 0.0f; else if (wy > 1.0f) wy = 1.0f;

        const float *lo_row = tile_lo + (size_t)iy * g;
        const float *inv_row = tile_inv + (size_t)iy * g;
        const uint8_t *uni_row = tile_uni + (size_t)iy * g;
        const float *lo_row2 = lo_row + g;
        const float *inv_row2 = inv_row + g;
        const uint8_t *uni_row2 = uni_row + g;
        float *sig_row = sig + (size_t)y * w;

        for (int32_t x = 0; x < w; ++x) {
            int32_t ix = ixs[x];
            float wx = wxs[x];

            float s = sig_row[x];
            float acc = 0.0f;
            for (int32_t dxi = 0; dxi < 2; ++dxi) {
                int32_t c = ix + dxi;
                float wxw = dxi ? wx : 1.0f - wx;
                float a, b;
                if (uni_row[c]) a = s;
                else {
                    a = (s - lo_row[c]) * inv_row[c];
                    if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
                }
                if (uni_row2[c]) b = s;
                else {
                    b = (s - lo_row2[c]) * inv_row2[c];
                    if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
                }
                acc += a * ((1.0f - wy) * wxw) + b * (wy * wxw);
            }
            sig_row[x] = acc < 0.0f ? 0.0f : (acc > 1.0f ? 1.0f : acc);
        }
    }

    free(ixs); free(wxs);
    free(ys); free(xs); free(tile_lo); free(tile_inv); free(tile_uni);
    return 0;
}

static void fill_levels(const uint8_t *rgb, int32_t w, int32_t h,
                        const carto_cell_opts *o, int32_t levels,
                        float *sig, float *scratch, uint8_t *fill) {
    int32_t n = w * h;
    luminance(rgb, n, sig);

    int32_t orient = o->orientation;
    if (orient != CARTO_ORIENT_DARK && orient != CARTO_ORIENT_BRIGHT) {
        double mean = 0.0;
        for (int32_t i = 0; i < n; ++i) mean += sig[i];
        orient = (mean / (double)n) < 0.4 ? CARTO_ORIENT_DARK : CARTO_ORIENT_BRIGHT;
    }
    if (orient == CARTO_ORIENT_BRIGHT)
        for (int32_t i = 0; i < n; ++i) sig[i] = 1.0f - sig[i];

    if (o->threshold_mode == CARTO_THRESH_ADAPTIVE) {
        if (adaptive_stretch(sig, w, h, scratch, o->tile_grid,
                             o->black_pct, o->white_pct, o->signal_floor) != 0)
            global_stretch(sig, n, scratch, o->black_pct, o->white_pct);
    } else {
        global_stretch(sig, n, scratch, o->black_pct, o->white_pct);
    }

    float gamma = o->signal_gamma < 0.05f ? 0.05f : o->signal_gamma;
    float scale = (float)(levels - 1);
    for (int32_t i = 0; i < n; ++i) {
        float v = sig[i];
        if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
        v = powf(v, gamma);
        sig[i] = v;
        /* rintf is round-half-to-even, which is what np.round does. */
        int32_t q = (int32_t)rintf(v * scale);
        if (q < 0) q = 0; else if (q > levels - 1) q = levels - 1;
        fill[i] = (uint8_t)q;
    }
}

/* ------------------------------------------------------------------- output */

static uint32_t pack(int32_t r, int32_t g, int32_t b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int carto_cellify(const uint8_t *rgb, int32_t w, int32_t h,
                  const carto_cell_opts *o,
                  uint32_t *glyph, uint32_t *fg, uint32_t *bg) {
    if (!rgb || !o || !glyph || o->cols < 1 || o->rows < 1) return -1;
    int32_t cw, ch;
    carto_cell_geometry(o->mode, &cw, &ch);
    if (w != o->cols * cw || h != o->rows * ch) return -1;

    const int32_t cols = o->cols, rows = o->rows;
    const int32_t want_color = o->want_color && fg;

    /* Half block needs no tone analysis at all: the two subcells are the two
     * colours, and the glyph never varies. */
    if (o->mode == CARTO_CELL_HALF) {
        for (int32_t y = 0; y < rows; ++y) {
            for (int32_t x = 0; x < cols; ++x) {
                int32_t c = y * cols + x;
                glyph[c] = 0x2580;
                if (!want_color) continue;
                const uint8_t *t = rgb + ((size_t)(2 * y) * w + x) * 3;
                const uint8_t *b = rgb + ((size_t)(2 * y + 1) * w + x) * 3;
                if (o->mono) {
                    int32_t tl = (int32_t)((0.299 * t[0] + 0.587 * t[1] + 0.114 * t[2]));
                    int32_t bl = (int32_t)((0.299 * b[0] + 0.587 * b[1] + 0.114 * b[2]));
                    fg[c] = pack(tl, tl, tl);
                    if (bg) bg[c] = pack(bl, bl, bl);
                } else {
                    fg[c] = pack(t[0], t[1], t[2]);
                    if (bg) bg[c] = pack(b[0], b[1], b[2]);
                }
            }
        }
        return 0;
    }

    int32_t levels = o->palette_len < 2 ? 2 : o->palette_len;
    int32_t n = w * h;
    float *sig = (float *)malloc((size_t)n * sizeof(float));
    float *scratch = (float *)malloc((size_t)n * sizeof(float));
    uint8_t *fill = (uint8_t *)malloc((size_t)n);
    if (!sig || !scratch || !fill) {
        free(sig); free(scratch); free(fill);
        return -1;
    }
    fill_levels(rgb, w, h, o, levels, sig, scratch, fill);

    const uint32_t *pal = o->palette;

    if (o->mode == CARTO_CELL_ASCII) {
        for (int32_t c = 0; c < cols * rows; ++c) {
            glyph[c] = pal[fill[c]];
            if (want_color) {
                const uint8_t *p = rgb + (size_t)c * 3;
                fg[c] = pack(p[0], p[1], p[2]);
            }
        }
    } else if (o->mode == CARTO_CELL_QUADRANT) {
        for (int32_t y = 0; y < rows; ++y) {
            for (int32_t x = 0; x < cols; ++x) {
                int32_t c = y * cols + x;
                size_t o0 = (size_t)(2 * y) * w + 2 * x;
                size_t o1 = (size_t)(2 * y + 1) * w + 2 * x;
                int32_t tl = fill[o0], tr = fill[o0 + 1];
                int32_t bl = fill[o1], br = fill[o1 + 1];
                int32_t mx = tl > tr ? tl : tr; int32_t m2 = bl > br ? bl : br;
                if (m2 > mx) mx = m2;
                int32_t mn = tl < tr ? tl : tr; int32_t m3 = bl < br ? bl : br;
                if (m3 < mn) mn = m3;
                int32_t avg = (tl + tr + bl + br) / 4;
                int32_t code = ((tl > avg) << 3) | ((tr > avg) << 2)
                             | ((bl > avg) << 1) | (br > avg);
                uint32_t gl = CARTO_QUAD_GLYPHS[code];
                if (mx == mn) {
                    int32_t fi = avg < 0 ? 0 : (avg > levels - 1 ? levels - 1 : avg);
                    gl = pal[fi];
                }
                if (mx == 0) gl = pal[0];
                if (mn >= levels - 1) gl = pal[levels - 1];
                if (o->shaded) {
                    int32_t partial = !(mx == mn) && !(mx == 0) && !(mn >= levels - 1);
                    int32_t half = levels / 2; if (half < 1) half = 1;
                    if (partial && avg >= half) {
                        int32_t si = avg < 1 ? 1 : (avg > levels - 1 ? levels - 1 : avg);
                        gl = pal[si];
                    }
                }
                glyph[c] = gl;
                if (want_color) {
                    const uint8_t *a = rgb + o0 * 3, *b = rgb + (o0 + 1) * 3;
                    const uint8_t *d = rgb + o1 * 3, *e = rgb + (o1 + 1) * 3;
                    fg[c] = pack((a[0] + b[0] + d[0] + e[0]) / 4,
                                 (a[1] + b[1] + d[1] + e[1]) / 4,
                                 (a[2] + b[2] + d[2] + e[2]) / 4);
                }
            }
        }
    } else { /* braille */
        for (int32_t y = 0; y < rows; ++y) {
            for (int32_t x = 0; x < cols; ++x) {
                int32_t c = y * cols + x;
                int32_t sum = 0;
                for (int32_t ry = 0; ry < 4; ++ry) {
                    size_t base = (size_t)(4 * y + ry) * w + 2 * x;
                    sum += fill[base] + fill[base + 1];
                }
                /* numpy takes this mean in float64; the comparison below is
                 * against the unrounded value. */
                double avg = (double)sum / 8.0;
                uint32_t code = 0;
                int32_t lit = 0;
                for (int32_t ry = 0; ry < 4; ++ry) {
                    size_t base = (size_t)(4 * y + ry) * w + 2 * x;
                    for (int32_t cx = 0; cx < 2; ++cx) {
                        if ((double)fill[base + cx] > avg) {
                            code |= CARTO_BRAILLE_BITS[ry][cx];
                            lit++;
                        }
                    }
                }
                uint32_t gl = CARTO_BRAILLE_BASE + code;
                if (code == 0 || code == 0xFF) {
                    int32_t fi = (int32_t)avg;  /* astype(int32) truncates */
                    if (fi < 0) fi = 0; else if (fi > levels - 1) fi = levels - 1;
                    gl = pal[fi];
                }
                if (o->shaded && lit >= 6) {
                    int32_t si = (int32_t)avg;
                    if (si < 1) si = 1; else if (si > levels - 1) si = levels - 1;
                    gl = pal[si];
                }
                glyph[c] = gl;

                if (want_color) {
                    /* Mean of the lit subcells, or of all eight when none are. */
                    int32_t acc[3] = {0, 0, 0}, whole[3] = {0, 0, 0};
                    for (int32_t ry = 0; ry < 4; ++ry) {
                        size_t base = (size_t)(4 * y + ry) * w + 2 * x;
                        for (int32_t cx = 0; cx < 2; ++cx) {
                            const uint8_t *p = rgb + (base + cx) * 3;
                            int32_t on = ((double)fill[base + cx] > avg);
                            for (int32_t k = 0; k < 3; ++k) {
                                whole[k] += p[k];
                                if (on) acc[k] += p[k];
                            }
                        }
                    }
                    int32_t out[3];
                    for (int32_t k = 0; k < 3; ++k) {
                        double v = lit > 0 ? (double)acc[k] / (double)lit
                                           : (double)whole[k] / 8.0;
                        if (v < 0.0) v = 0.0; else if (v > 255.0) v = 255.0;
                        out[k] = (int32_t)v;  /* astype(uint8) truncates */
                    }
                    fg[c] = pack(out[0], out[1], out[2]);
                }
            }
        }
    }

    free(sig); free(scratch); free(fill);
    return 0;
}

/* --------------------------------------------------- RGB565 source pipeline */

/* Pillow's two downsamplers, reproduced exactly so the fused path and the PIL
 * path agree bit for bit. Which one applies is decided the same way
 * renderer._resample decides it. */

#define CARTO_PREC_BITS 22

/* The table is little-endian RGBA -- red in the low byte -- because the same
 * table is handed to PIL as a raw RGBA buffer on the non-fused path. */
static inline void lut_rgb(const uint32_t *lut, uint16_t v, int32_t *out) {
    uint32_t c = lut[v];
    out[0] = (int32_t)(c & 0xFF);
    out[1] = (int32_t)((c >> 8) & 0xFF);
    out[2] = (int32_t)((c >> 16) & 0xFF);
}

/* ImagingReduceNxN: block sum biased by half a block, then a reciprocal
 * multiply. The reciprocal is computed in float, as Pillow does. */
static void reduce_nxn_565(const uint16_t *src, int32_t sw, const uint32_t *lut,
                           int32_t fx, int32_t fy, uint8_t *dst,
                           int32_t dw, int32_t dh) {
    int32_t n = fx * fy;
    uint32_t multiplier =
        (uint32_t)(((float)(1 << 30) * 4.0f) / (float)((1 << 8) * n));
    uint32_t amend = (uint32_t)(n / 2);
    for (int32_t y = 0; y < dh; ++y) {
        for (int32_t x = 0; x < dw; ++x) {
            uint32_t s0 = amend, s1 = amend, s2 = amend;
            for (int32_t yy = 0; yy < fy; ++yy) {
                const uint16_t *row = src + (size_t)(y * fy + yy) * sw + x * fx;
                for (int32_t xx = 0; xx < fx; ++xx) {
                    int32_t c[3];
                    lut_rgb(lut, row[xx], c);
                    s0 += (uint32_t)c[0];
                    s1 += (uint32_t)c[1];
                    s2 += (uint32_t)c[2];
                }
            }
            uint8_t *o = dst + ((size_t)y * dw + x) * 3;
            o[0] = (uint8_t)((s0 * multiplier) >> 24);
            o[1] = (uint8_t)((s1 * multiplier) >> 24);
            o[2] = (uint8_t)((s2 * multiplier) >> 24);
        }
    }
}

/* precompute_coeffs for the box filter, normalised and fixed-point encoded the
 * way normalize_coeffs_8bpc does. */
static int32_t box_coeffs(int32_t in_size, int32_t out_size,
                          int32_t **bounds_out, int32_t **kk_out) {
    double scale = (double)in_size / (double)out_size;
    double fscale = scale < 1.0 ? 1.0 : scale;
    double support = 0.5 * fscale;
    double inv = 1.0 / fscale;
    int32_t ksize = (int32_t)ceil(support) * 2 + 1;

    int32_t *bounds = (int32_t *)malloc((size_t)out_size * 2 * sizeof(int32_t));
    int32_t *kk = (int32_t *)calloc((size_t)out_size * ksize, sizeof(int32_t));
    double *tmp = (double *)malloc((size_t)ksize * sizeof(double));
    if (!bounds || !kk || !tmp) {
        free(bounds); free(kk); free(tmp);
        return -1;
    }

    for (int32_t xx = 0; xx < out_size; ++xx) {
        double center = ((double)xx + 0.5) * scale;
        int32_t xmin = (int32_t)(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int32_t xmax = (int32_t)(center + support + 0.5);
        if (xmax > in_size) xmax = in_size;
        xmax -= xmin;
        double ww = 0.0;
        for (int32_t x = 0; x < xmax; ++x) {
            double arg = ((double)(x + xmin) - center + 0.5) * inv;
            double w = (arg > -0.5 && arg <= 0.5) ? 1.0 : 0.0;
            tmp[x] = w;
            ww += w;
        }
        bounds[xx * 2] = xmin;
        bounds[xx * 2 + 1] = xmax;
        for (int32_t x = 0; x < xmax; ++x) {
            double w = ww != 0.0 ? tmp[x] / ww : 0.0;
            kk[xx * ksize + x] =
                (int32_t)(0.5 + w * (double)(1 << CARTO_PREC_BITS));
        }
    }
    free(tmp);
    *bounds_out = bounds;
    *kk_out = kk;
    return ksize;
}

static inline uint8_t clip8_prec(int64_t v) {
    v >>= CARTO_PREC_BITS;
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* Horizontal then vertical, each rounding to 8 bits, exactly as Pillow does. */
static int box_resize_565(const uint16_t *src, int32_t sw, int32_t sh,
                          const uint32_t *lut, uint8_t *dst,
                          int32_t dw, int32_t dh) {
    int32_t *hb = NULL, *hk = NULL, *vb = NULL, *vk = NULL;
    int32_t hks = box_coeffs(sw, dw, &hb, &hk);
    int32_t vks = box_coeffs(sh, dh, &vb, &vk);
    uint8_t *mid = (uint8_t *)malloc((size_t)dw * sh * 3);
    if (hks < 0 || vks < 0 || !mid) {
        free(hb); free(hk); free(vb); free(vk); free(mid);
        return -1;
    }
    const int64_t bias = (int64_t)1 << (CARTO_PREC_BITS - 1);

    for (int32_t y = 0; y < sh; ++y) {
        const uint16_t *row = src + (size_t)y * sw;
        uint8_t *out = mid + (size_t)y * dw * 3;
        for (int32_t x = 0; x < dw; ++x) {
            int32_t xmin = hb[x * 2], n = hb[x * 2 + 1];
            const int32_t *k = hk + (size_t)x * hks;
            int64_t a = bias, b = bias, c = bias;
            for (int32_t i = 0; i < n; ++i) {
                int32_t px[3];
                lut_rgb(lut, row[xmin + i], px);
                a += (int64_t)px[0] * k[i];
                b += (int64_t)px[1] * k[i];
                c += (int64_t)px[2] * k[i];
            }
            out[x * 3] = clip8_prec(a);
            out[x * 3 + 1] = clip8_prec(b);
            out[x * 3 + 2] = clip8_prec(c);
        }
    }

    for (int32_t y = 0; y < dh; ++y) {
        int32_t ymin = vb[y * 2], n = vb[y * 2 + 1];
        const int32_t *k = vk + (size_t)y * vks;
        uint8_t *out = dst + (size_t)y * dw * 3;
        for (int32_t x = 0; x < dw; ++x) {
            int64_t a = bias, b = bias, c = bias;
            for (int32_t i = 0; i < n; ++i) {
                const uint8_t *p = mid + ((size_t)(ymin + i) * dw + x) * 3;
                a += (int64_t)p[0] * k[i];
                b += (int64_t)p[1] * k[i];
                c += (int64_t)p[2] * k[i];
            }
            out[x * 3] = clip8_prec(a);
            out[x * 3 + 1] = clip8_prec(b);
            out[x * 3 + 2] = clip8_prec(c);
        }
    }

    free(hb); free(hk); free(vb); free(vk); free(mid);
    return 0;
}

int carto_cellify_rgb565(const uint16_t *src, int32_t sw, int32_t sh,
                         const uint32_t *lut, const carto_cell_opts *opts,
                         uint32_t *glyph, uint32_t *fg, uint32_t *bg) {
    if (!src || !lut || !opts || !glyph) return -1;
    int32_t cw, chh;
    carto_cell_geometry(opts->mode, &cw, &chh);
    int32_t dw = opts->cols * cw, dh = opts->rows * chh;
    if (dw < 1 || dh < 1) return -1;
    /* Upscaling wants Lanczos rather than a box; let the caller fall back. */
    if (sw < dw || sh < dh) return -1;

    uint8_t *rgb = (uint8_t *)malloc((size_t)dw * dh * 3);
    if (!rgb) return -1;

    int rc;
    if (sw == dw && sh == dh) {
        for (int32_t i = 0; i < dw * dh; ++i) {
            int32_t c[3];
            lut_rgb(lut, src[i], c);
            rgb[i * 3] = (uint8_t)c[0];
            rgb[i * 3 + 1] = (uint8_t)c[1];
            rgb[i * 3 + 2] = (uint8_t)c[2];
        }
        rc = 0;
    } else if (sw % dw == 0 && sh % dh == 0 && (sw / dw > 1 || sh / dh > 1)) {
        reduce_nxn_565(src, sw, lut, sw / dw, sh / dh, rgb, dw, dh);
        rc = 0;
    } else {
        rc = box_resize_565(src, sw, sh, lut, rgb, dw, dh);
    }
    if (rc == 0)
        rc = carto_cellify(rgb, dw, dh, opts, glyph, fg, bg);
    free(rgb);
    return rc;
}

/* Colour histogram over the framebuffer.
 *
 * np.bincount widens its input to intp before counting, so a million 16-bit
 * indices become an 8 MB temporary first. Counting them where they lie costs a
 * single pass. `bins` must hold 65536 entries and is written, not accumulated.
 */
void carto_histogram_u16(const uint16_t *src, int64_t n, int64_t *bins) {
    if (!src || !bins || n < 0) return;
    memset(bins, 0, 65536 * sizeof(int64_t));
    for (int64_t i = 0; i < n; ++i)
        bins[src[i]]++;
}
