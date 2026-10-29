#ifndef LR_IQ_SPECTRUM_H
#define LR_IQ_SPECTRUM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define LR_SPEC_BINS 256
#define LR_SPEC_PERIOD_MS 100
typedef struct {
    uint32_t center_hz, sequence, captured_ms;
    bool gap;
} lr_spec_line;
/* UI enables/reads; the single RX owner begins/feeds/ends after checking blocks.
 * The FM NBFM demodulator consumes these same owner-held blocks before release,
 * never a second capture or queue consumer. Spectrum work is expendable. */
/* Atomic request only; first spare RX block initializes retained FFT tables. */
bool lr_spec_enable(bool on);
void lr_spec_begin(uint32_t hz);
void lr_spec_end(void);
uint32_t lr_spec_center(void);
/* No waits or allocation. spare=false skips FFT work, not decoder samples. */
void lr_spec_feed(const uint8_t *data, size_t bytes, uint64_t first_pair,
                  uint32_t generation, uint32_t now_ms, bool spare);
bool lr_spec_read(float *bins, lr_spec_line *line, uint32_t now_ms);
/* Console counters: lines published, lines marked as spanning a gap, blocks
   the owner had no spare time for. */
void lr_spec_stats(bool *on, uint32_t *center_hz, uint32_t *lines, uint32_t *gaps,
                   uint32_t *skipped);

/* WIDE SPAN: the whole wide route (576k around the LO), fed by the wide
 * session's owner with the expanded samples before its down-converter.
 * The LO sits in the middle (its DC leakage with it); the channel sits
 * offset above it. Same mailbox shape as the channel spectrum above, its
 * own PSRAM tables; four FFT windows averaged into a 100 ms row. */
#define LR_SPAN_BINS 256
typedef struct {
    uint32_t lo_hz, channel_hz, span_hz, sequence, captured_ms;
    bool gap;
} lr_span_line;
typedef struct { bool on; uint32_t lines, windows, skipped; lr_span_line last; } lr_span_stats_t;
bool lr_span_enable(bool on);
bool lr_span_enabled(void);
/* Owner only. No waits or allocation; nothing at all while disabled. */
void lr_span_feed(const int16_t *iq, size_t pairs, uint32_t lo_hz, uint32_t channel_hz,
                  uint32_t rate_hz, uint32_t now_ms, bool spare);
/* The run ended: the next line starts a new picture. */
void lr_span_end(void);
/* Bins 0..1 over -100..0 dBFS, low frequency first. */
bool lr_span_read(float *bins, lr_span_line *line, uint32_t now_ms);
void lr_span_stats(lr_span_stats_t *out);
#endif
