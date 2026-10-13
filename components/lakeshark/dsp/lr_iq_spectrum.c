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
