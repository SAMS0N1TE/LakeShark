#include "fm_spectrum.h"
#include "spectrum.h"
#include "esp_attr.h"
#include <string.h>
#include <math.h>

static EXT_RAM_BSS_ATTR float s_work[FM_SPECTRUM_BINS];
static EXT_RAM_BSS_ATTR float s_bins[FM_SPECTRUM_BINS];
static bool s_enabled;
static uint32_t s_epoch, s_published_epoch, s_lock;
static fm_spectrum_snapshot_t s_meta;

/* Zoom. The worker mixes the view centre down to 0 Hz, decimates by half
   the factor with a third-order CIC, and transforms 512 of those samples.
   Only the middle half of that transform is shown: there the CIC droops by
   under 3 dB (put back per bin) and what folds in from outside is about
   30 dB down. Its own FFT context, so the shared one is not disturbed. */
static unsigned s_zoom = 1;
static uint32_t s_view_hz;
static EXT_RAM_BSS_ATTR spectrum_t s_zctx;
static EXT_RAM_BSS_ATTR float s_zdb[SPEC_FFT_N];

bool fm_spectrum_zoom(unsigned factor, uint32_t view_hz)
{
    if (factor != 1 && factor != 2 && factor != 4 && factor != 8 &&
        factor != 16 && factor != 32) return false;
    __atomic_store_n(&s_view_hz, factor > 1 ? view_hz : 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_zoom, factor, __ATOMIC_RELEASE);
    fm_spectrum_invalidate();
    return true;
}
unsigned fm_spectrum_zoom_factor(void) { return __atomic_load_n(&s_zoom, __ATOMIC_ACQUIRE); }

/* The CIC's response at f (Hz) for decimation r at input rate fs, as power
   in dB: |h|^3 in amplitude, squared. */
static float cic_db(float f, int r, float fs)
{
    if (r <= 1 || f == 0) return 0;
    const float x = (float)M_PI * f / fs;
    const float h = sinf(x * r) / (r * sinf(x));
    return 60.0f * log10f(fabsf(h) > 1e-6f ? fabsf(h) : 1e-6f);
}

/* One zoomed frame into out[FM_SPECTRUM_BINS] as dBFS. False when the block
   is too short for this factor. */
static bool zoom_frame(const uint8_t *iq, int len, unsigned zoom, int32_t offset_hz,
                       uint32_t rate, float *out)
{
    const int r = (int)zoom / 2;           /* CIC decimation; 1 at 2x */
    const int need = SPEC_FFT_N * r;
    if (len / 2 < need) return false;
    if (!s_zctx.ready) spectrum_context_init(&s_zctx);
    spectrum_context_reset(&s_zctx);
    /* NCO at -offset, kept on the unit circle by a renormalising rotation. */
    const double w = -2.0 * M_PI * (double)offset_hz / (double)rate;
    const float sr = (float)sin(w), cr = (float)cos(w);
    float pr = 1.0f, pi = 0.0f;
    double i1r = 0, i1i = 0, i2r = 0, i2i = 0, i3r = 0, i3i = 0;
    double c1r = 0, c1i = 0, c2r = 0, c2i = 0, c3r = 0, c3i = 0;
    const double gain = 1.0 / ((double)r * r * r);
    int k = 0;
    for (int n = 0; n < need; ++n) {
        const float xr = ((float)iq[2 * n] - 127.5f) * (1.0f / 127.5f);
        const float xi = ((float)iq[2 * n + 1] - 127.5f) * (1.0f / 127.5f);
        const float yr = xr * pr - xi * pi, yi = xr * pi + xi * pr;
        const float npr = pr * cr - pi * sr, npi = pr * sr + pi * cr;
        pr = npr; pi = npi;
        if ((n & 255) == 255) { const float m = 1.0f / sqrtf(pr * pr + pi * pi); pr *= m; pi *= m; }
        if (r == 1) {
            const float wv = s_zctx.win[k];
            s_zctx.re[k] = yr * wv; s_zctx.im[k] = yi * wv; ++k;
            continue;
        }
        i1r += yr; i1i += yi; i2r += i1r; i2i += i1i; i3r += i2r; i3i += i2i;
        if ((n + 1) % r) continue;
        double d1r = i3r - c1r, d1i = i3i - c1i; c1r = i3r; c1i = i3i;
        double d2r = d1r - c2r, d2i = d1i - c2i; c2r = d1r; c2i = d1i;
        double d3r = d2r - c3r, d3i = d2i - c3i; c3r = d2r; c3i = d2i;
        const float wv = s_zctx.win[k];
        s_zctx.re[k] = (float)(d3r * gain) * wv;
        s_zctx.im[k] = (float)(d3i * gain) * wv;
        ++k;
    }
    spectrum_context_accum(&s_zctx);
    if (!spectrum_context_read_db(&s_zctx, s_zdb, SPEC_FFT_N)) return false;
    /* The middle half; each bin with the CIC's droop at its offset put back. */
    const float fs_out = (float)rate / (float)r, bin_hz = fs_out / SPEC_FFT_N;
    for (int i = 0; i < FM_SPECTRUM_BINS; ++i) {
        const int src = SPEC_FFT_N / 4 + i * (SPEC_FFT_N / 2) / FM_SPECTRUM_BINS;
        const float f = (src - SPEC_FFT_N / 2) * bin_hz;
        out[i] = s_zdb[src] - cic_db(f, r, (float)rate);
    }
    return true;
}

void fm_spectrum_enable(bool enabled)
{
    if (__atomic_exchange_n(&s_enabled, enabled, __ATOMIC_ACQ_REL) != enabled)
        fm_spectrum_invalidate();
}

void fm_spectrum_invalidate(void)
{
    __atomic_add_fetch(&s_epoch, 1, __ATOMIC_ACQ_REL);
}

/* The FM receive worker is the only writer and owns the shared FFT. */
void fm_spectrum_feed(const uint8_t *iq, int len, uint32_t center_hz,
                      uint32_t span_hz, uint32_t now_ms)
{
    if (!__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE) || !iq ||
        len < SPEC_FFT_N * 2 || !center_hz || !span_hz) return;
    const uint32_t epoch = __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE);
    if (s_meta.sequence && s_published_epoch == epoch &&
        s_meta.center_hz == center_hz && s_meta.span_hz == span_hz &&
        (uint32_t)(now_ms - s_meta.captured_ms) < FM_SPECTRUM_PERIOD_MS) return;
    const unsigned zoom = __atomic_load_n(&s_zoom, __ATOMIC_ACQUIRE);
    if (zoom > 1) {
        /* A view the span cannot hold, after a retune, goes back to centre. */
        int64_t off = 0;
        const uint32_t view = __atomic_load_n(&s_view_hz, __ATOMIC_ACQUIRE);
        if (view) off = (int64_t)view - (int64_t)center_hz;
        const int64_t reach = (int64_t)span_hz / 2 - (int64_t)span_hz / zoom / 2;
        if (off > reach || off < -reach) off = 0;
        if (!zoom_frame(iq, len, zoom, (int32_t)off, span_hz, s_work)) return;
        center_hz = (uint32_t)((int64_t)center_hz + off);
        span_hz /= zoom;
    } else {
        spectrum_reset();
        spectrum_accum(iq, len);
        if (!spectrum_read_db(s_work, FM_SPECTRUM_BINS)) return;
        spectrum_reset();
    }
    for (int i = 0; i < FM_SPECTRUM_BINS; ++i) {
        float v = (s_work[i] - FM_SPECTRUM_FLOOR_DB) /
                  (FM_SPECTRUM_TOP_DB - FM_SPECTRUM_FLOOR_DB);
        s_work[i] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
    __atomic_add_fetch(&s_lock, 1, __ATOMIC_ACQ_REL);
    memcpy(s_bins, s_work, sizeof(s_bins));
    s_meta.center_hz = center_hz;
    s_meta.span_hz = span_hz;
    s_meta.captured_ms = now_ms;
    ++s_meta.sequence;
    s_published_epoch = epoch;
    __atomic_add_fetch(&s_lock, 1, __ATOMIC_RELEASE);
}

bool fm_spectrum_read(float *bins, int n, uint32_t now_ms,
                      fm_spectrum_snapshot_t *snapshot)
{
    if (!bins || n != FM_SPECTRUM_BINS ||
        !__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE)) return false;
    const uint32_t epoch = __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t before = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
        if (before & 1) continue;
        fm_spectrum_snapshot_t meta = s_meta;
        const uint32_t published = s_published_epoch;
        memcpy(bins, s_bins, sizeof(s_bins));
        const uint32_t after = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
        if (before != after || (after & 1)) continue;
        if (!meta.sequence || published != epoch ||
            (uint32_t)(now_ms - meta.captured_ms) > 500) return false;
        if (snapshot) *snapshot = meta;
        return true;
    }
    return false;
}
