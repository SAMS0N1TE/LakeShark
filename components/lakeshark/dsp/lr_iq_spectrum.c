#include "lr_iq_spectrum.h"
#include "spectrum.h"
#include "esp_attr.h"
#include <stdatomic.h>
#include <string.h>

typedef struct {
    spectrum_t fft;
    float bins[LR_SPEC_BINS];
    lr_spec_line line;
    uint32_t published_epoch;
} storage_t;
static EXT_RAM_BSS_ATTR storage_t storage;
static storage_t *const s = &storage;
static atomic_bool ready;
static atomic_bool enabled;
static atomic_uint center, epoch;
static atomic_flag mailbox = ATOMIC_FLAG_INIT;
/* RX-owner-only state. */
static uint32_t seen_epoch, generation, last_fft, line_start;
static uint64_t expected;
static unsigned fill;
static bool have, gap;
static atomic_uint n_lines, n_gaps, n_skipped;

bool lr_spec_enable(bool on)
{
    /* UI request only. The RX owner initializes the retained FFT. */
    if (atomic_exchange(&enabled, on) != on) atomic_fetch_add(&epoch, 1);
    return true;
}
uint32_t lr_spec_center(void) { return atomic_load(&center); }
void lr_spec_begin(uint32_t hz)
{
    atomic_store(&center, hz);
    atomic_fetch_add(&epoch, 1);
    have = false; gap = true;
}
void lr_spec_end(void) { atomic_store(&center, 0); }

void lr_spec_feed(const uint8_t *data, size_t bytes, uint64_t first,
                  uint32_t gen, uint32_t now, bool spare)
{
    if (!atomic_load(&enabled)) return;
    if (!atomic_load_explicit(&ready, memory_order_acquire)) {
        if (!data || bytes % 4 || !spare) { atomic_fetch_add(&n_skipped, 1); return; }
        spectrum_context_init(&s->fft);
        atomic_store_explicit(&ready, true, memory_order_release);
    }
    const uint32_t e = atomic_load(&epoch);
    bool discontinuity = !have || generation != gen || first != expected;
    expected = first + bytes / 4; generation = gen; have = true;
    if (discontinuity) gap = true;
    /* Only this owner touches the FFT. UI contention never delays it. */
    if (seen_epoch != e || discontinuity) {
        spectrum_context_reset(&s->fft); fill = 0;
        seen_epoch = e; line_start = now; last_fft = now;
    }
    if (!data || bytes % 4 || !spare) { fill = 0; atomic_fetch_add(&n_skipped, 1); return; }
    /* One FFT window is enough for each 100 ms waterfall row. Running one
       every 20 ms only averaged five windows into a row the UI cannot display
       any sooner; it spent RX-owner time without increasing row cadence. */
    if (!fill && (uint32_t)(now - last_fft) < LR_SPEC_PERIOD_MS) return;
    for (size_t k = 0; k < bytes && fill < SPEC_FFT_N; k += 4) {
        const int16_t i = (int16_t)((uint16_t)data[k] | (uint16_t)data[k+1] << 8);
        const int16_t q = (int16_t)((uint16_t)data[k+2] | (uint16_t)data[k+3] << 8);
        float w = s->fft.win[fill] / 32768.0f;
        s->fft.re[fill] = i * w; s->fft.im[fill] = q * w; ++fill;
    }
    if (fill != SPEC_FFT_N) return;
    spectrum_context_accum(&s->fft); fill = 0; last_fft = now;
    if ((uint32_t)(now - line_start) < LR_SPEC_PERIOD_MS) return;
    if (atomic_flag_test_and_set_explicit(&mailbox, memory_order_acquire)) return;
    spectrum_context_read_db(&s->fft, s->bins, LR_SPEC_BINS);
    for (int i = 0; i < LR_SPEC_BINS; ++i) {
        float v = (s->bins[i] + 100.0f) / 100.0f;
        s->bins[i] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
    s->published_epoch = e;
    s->line.center_hz = atomic_load(&center);
    s->line.captured_ms = now; ++s->line.sequence; s->line.gap = gap || s->line.gap;
    atomic_fetch_add(&n_lines, 1); if (gap) atomic_fetch_add(&n_gaps, 1);
    atomic_flag_clear_explicit(&mailbox, memory_order_release);
    spectrum_context_reset(&s->fft); line_start = now; gap = false;
}

bool lr_spec_read(float *bins, lr_spec_line *line, uint32_t now)
{
    if (!atomic_load_explicit(&ready, memory_order_acquire) ||
        !atomic_load(&enabled) || !atomic_load(&center) || !bins || !line ||
        atomic_flag_test_and_set_explicit(&mailbox, memory_order_acquire)) return false;
    bool ok = s->published_epoch == atomic_load(&epoch) && s->line.sequence && s->line.center_hz == atomic_load(&center) &&
              (uint32_t)(now - s->line.captured_ms) <= 500;
    if (ok) { memcpy(bins, s->bins, sizeof(s->bins)); *line = s->line; s->line.gap = false; }
    atomic_flag_clear_explicit(&mailbox, memory_order_release);
    return ok;
}

void lr_spec_stats(bool *on, uint32_t *center_hz, uint32_t *lines, uint32_t *gaps,
                   uint32_t *skipped)
{
    *on = atomic_load(&enabled); *center_hz = atomic_load(&center);
    *lines = atomic_load(&n_lines); *gaps = atomic_load(&n_gaps); *skipped = atomic_load(&n_skipped);
}

/* ---- WIDE SPAN ---- */
/* Four windows a row: at 576k one 512-pair window is under 1 ms of a 100 ms
   row, and a single one leaves the noise floor speckled across 576 kHz. */
#define SPAN_WINDOWS 4
typedef struct {
    spectrum_t fft;
    float bins[LR_SPAN_BINS];
    lr_span_line line;
    uint32_t published_epoch;
} span_storage_t;
static EXT_RAM_BSS_ATTR span_storage_t span_storage;
static span_storage_t *const sp = &span_storage;
static atomic_bool span_on, span_ready;
static atomic_uint span_epoch, span_lines, span_windows, span_skipped;
static atomic_flag span_box = ATOMIC_FLAG_INIT;
/* Owner-only. */
static uint32_t span_seen, span_last_win, span_line_start, span_nwin, span_lo, span_ch;
static bool span_gap;

bool lr_span_enable(bool on)
{
    if (atomic_exchange(&span_on, on) != on) atomic_fetch_add(&span_epoch, 1);
    return true;
}
bool lr_span_enabled(void) { return atomic_load(&span_on); }
void lr_span_end(void) { atomic_fetch_add(&span_epoch, 1); }

void lr_span_feed(const int16_t *iq, size_t pairs, uint32_t lo_hz, uint32_t channel_hz,
                  uint32_t rate_hz, uint32_t now, bool spare)
{
    if (!atomic_load_explicit(&span_on, memory_order_relaxed) || !iq || !rate_hz) return;
    if (!atomic_load_explicit(&span_ready, memory_order_acquire)) {
        if (!spare) { atomic_fetch_add(&span_skipped, 1); return; }
        spectrum_context_init(&sp->fft);
        atomic_store_explicit(&span_ready, true, memory_order_release);
    }
    const uint32_t e = atomic_load(&span_epoch);
    if (span_seen != e || lo_hz != span_lo || channel_hz != span_ch) {
        spectrum_context_reset(&sp->fft);
        span_seen = e; span_lo = lo_hz; span_ch = channel_hz;
        span_nwin = 0; span_line_start = now; span_last_win = now - LR_SPEC_PERIOD_MS;
        span_gap = true;
    }
    if (!spare) { atomic_fetch_add(&span_skipped, 1); return; }
    /* A window needs one call's worth: the session hands 512-pair slots. */
    if (pairs < SPEC_FFT_N ||
        (uint32_t)(now - span_last_win) < LR_SPEC_PERIOD_MS / SPAN_WINDOWS) return;
    for (unsigned k = 0; k < SPEC_FFT_N; ++k) {
        const float w = sp->fft.win[k] / 32768.0f;
        sp->fft.re[k] = iq[2 * k] * w; sp->fft.im[k] = iq[2 * k + 1] * w;
    }
    spectrum_context_accum(&sp->fft);
    ++span_nwin; span_last_win = now; atomic_fetch_add(&span_windows, 1);
    if ((uint32_t)(now - span_line_start) < LR_SPEC_PERIOD_MS) return;
    if (atomic_flag_test_and_set_explicit(&span_box, memory_order_acquire)) return;
    spectrum_context_read_db(&sp->fft, sp->bins, LR_SPAN_BINS);
    for (int i = 0; i < LR_SPAN_BINS; ++i) {
        const float v = (sp->bins[i] + 100.0f) / 100.0f;
        sp->bins[i] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
    sp->published_epoch = e;
    sp->line.lo_hz = lo_hz; sp->line.channel_hz = channel_hz; sp->line.span_hz = rate_hz;
    sp->line.captured_ms = now; ++sp->line.sequence; sp->line.gap = span_gap || sp->line.gap;
    atomic_flag_clear_explicit(&span_box, memory_order_release);
    atomic_fetch_add(&span_lines, 1);
    spectrum_context_reset(&sp->fft); span_nwin = 0; span_line_start = now; span_gap = false;
}

bool lr_span_read(float *bins, lr_span_line *line, uint32_t now)
{
    if (!atomic_load_explicit(&span_ready, memory_order_acquire) || !atomic_load(&span_on) ||
        !bins || !line || atomic_flag_test_and_set_explicit(&span_box, memory_order_acquire)) return false;
    const bool ok = sp->published_epoch == atomic_load(&span_epoch) && sp->line.sequence &&
                    (uint32_t)(now - sp->line.captured_ms) <= 500;
    if (ok) { memcpy(bins, sp->bins, sizeof(sp->bins)); *line = sp->line; sp->line.gap = false; }
    atomic_flag_clear_explicit(&span_box, memory_order_release);
    return ok;
}

void lr_span_stats(lr_span_stats_t *out)
{
    *out = (lr_span_stats_t){
        .on = atomic_load(&span_on), .lines = atomic_load(&span_lines),
        .windows = atomic_load(&span_windows), .skipped = atomic_load(&span_skipped),
    };
    if (!atomic_flag_test_and_set_explicit(&span_box, memory_order_acquire)) {
        out->last = sp->line;
        atomic_flag_clear_explicit(&span_box, memory_order_release);
    }
}
