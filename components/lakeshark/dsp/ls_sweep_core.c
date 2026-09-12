/* See ls_sweep_core.h. Pure arithmetic; no radio, no ESP-IDF. */

#include "ls_sweep_core.h"

#include <string.h>

bool ls_sweep_plan(uint64_t start_hz, uint64_t stop_hz, uint32_t bin_hz,
                   uint32_t sample_rate_hz, ls_sweep_plan_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    if (stop_hz <= start_hz)  return false;
    if (bin_hz == 0)          return false;
    if (sample_rate_hz == 0)  return false;

    /* Trusted distance either side of the tune centre. */
    uint32_t half_span = (uint32_t)(((uint64_t)sample_rate_hz *
                                     LS_SWEEP_USABLE_PCT) / 200u);
    if (half_span <= LS_SWEEP_DC_GUARD_HZ) return false;

    /* Single sideband: everything above the DC guard, up to the edge of the
       trusted region. See the header for why the two-sided version leaves a
       hole at every tune centre. */
    uint32_t strip = half_span - LS_SWEEP_DC_GUARD_HZ;

    /* A bin wider than one strip can never be filled by a single tune. */
    if (bin_hz > strip) return false;

    uint64_t span = stop_hz - start_hz;

    /* Round the span up to a whole number of bins so the last bin is not a
       partial one that only ever gets part of the energy of its neighbours. */
    uint64_t n_bins = (span + bin_hz - 1u) / bin_hz;
    if (n_bins == 0 || n_bins > LS_SWEEP_MAX_BINS) return false;

    uint64_t n_tunes = (span + strip - 1u) / strip;
    if (n_tunes == 0) return false;

    out->start_hz       = start_hz;
    out->stop_hz        = stop_hz;
    out->bin_hz         = bin_hz;
    out->n_bins         = (uint32_t)n_bins;
    out->sample_rate_hz = sample_rate_hz;
    out->half_span_hz   = half_span;
    out->strip_hz       = strip;
    out->n_tunes        = (uint32_t)n_tunes;
    return true;
}

uint64_t ls_sweep_tune_center(const ls_sweep_plan_t *plan, uint32_t i)
{
    if (!plan || i >= plan->n_tunes) return 0;
    /* Strip i covers [start + i*strip, start + (i+1)*strip), and the strip
       begins one DC guard above the tune centre. */
    return plan->start_hz + (uint64_t)plan->strip_hz * i -
           LS_SWEEP_DC_GUARD_HZ;
}

int64_t ls_sweep_bin_hz(const ls_sweep_plan_t *plan, uint64_t center_hz,
                        int fft_bin, int fft_n)
{
    if (!plan || fft_n <= 0) return 0;
    /* spectrum_read_db is fftshifted: index 0 is centre - rate/2. */
    int64_t offset_bins = (int64_t)fft_bin - (int64_t)(fft_n / 2);
    int64_t offset_hz   = (offset_bins * (int64_t)plan->sample_rate_hz) /
                          (int64_t)fft_n;
    return (int64_t)center_hz + offset_hz;
}

int ls_sweep_bin_index(const ls_sweep_plan_t *plan, uint64_t center_hz,
                       int fft_bin, int fft_n)
{
    if (!plan || fft_n <= 0 || fft_bin < 0 || fft_bin >= fft_n) return -1;

    int64_t hz = ls_sweep_bin_hz(plan, center_hz, fft_bin, fft_n);
    if (hz < 0) return -1;

    /* Above this tune's DC spike, and inside its trusted region. Everything
       below centre belongs to a lower tune; the guard band and the analogue
       roll-off belong to nobody. */
    int64_t above = hz - (int64_t)center_hz;
    if (above < (int64_t)LS_SWEEP_DC_GUARD_HZ) return -1;
    if (above >= (int64_t)plan->half_span_hz)  return -1;

    if ((uint64_t)hz < plan->start_hz) return -1;
    uint64_t rel = (uint64_t)hz - plan->start_hz;
    uint64_t idx = rel / plan->bin_hz;
    if (idx >= plan->n_bins) return -1;

    return (int)idx;
}

void ls_sweep_reset(const ls_sweep_plan_t *plan, int8_t *dbfs)
{
    if (!plan || !dbfs) return;
    memset(dbfs, LS_SWEEP_NO_DATA, plan->n_bins);
}

void ls_sweep_fold(const ls_sweep_plan_t *plan, uint64_t center_hz,
                   const float *db, int fft_n, int8_t *dbfs)
{
    if (!plan || !db || !dbfs || fft_n <= 0) return;

    for (int k = 0; k < fft_n; k++) {
        int idx = ls_sweep_bin_index(plan, center_hz, k, fft_n);
        if (idx < 0) continue;

        float v = db[k];
        /* dBFS is negative. Clamp into int8 leaving -128 free as the sentinel,
           so a genuinely dead bin can never be mistaken for one no tune
           reached. */
        if (v > 127.0f)  v = 127.0f;
        if (v < -127.0f) v = -127.0f;
        int8_t q = (int8_t)(v < 0.0f ? (v - 0.5f) : (v + 0.5f));

        if (dbfs[idx] == LS_SWEEP_NO_DATA || q > dbfs[idx]) dbfs[idx] = q;
    }
}

uint64_t ls_sweep_out_hz(const ls_sweep_plan_t *plan, uint32_t i)
{
    if (!plan || i >= plan->n_bins) return 0;
    return plan->start_hz + (uint64_t)plan->bin_hz * i + plan->bin_hz / 2u;
}

uint32_t ls_sweep_gaps(const ls_sweep_plan_t *plan, const int8_t *dbfs)
{
    if (!plan || !dbfs) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < plan->n_bins; i++)
        if (dbfs[i] == LS_SWEEP_NO_DATA) n++;
    return n;
}
