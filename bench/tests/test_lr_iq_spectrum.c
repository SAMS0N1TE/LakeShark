#include "ls_test.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <string.h>
/* Include the producer to exercise mailbox contention deterministically. */
#include "../../components/lakeshark/dsp/lr_iq_spectrum.c"
static uint8_t raw[1280];
static uint64_t pair;
static uint32_t ms;
static unsigned fft_count;
static float bins[LR_SPEC_BINS];
static lr_spec_line line;
static void block(int bin, uint32_t gen, bool spare)
{
    for (unsigned k = 0; k < sizeof(raw)/4; ++k) {
        double a = 6.283185307179586 * bin * (double)(pair+k) / SPEC_FFT_N;
        int16_t i = (int16_t)(16000*cos(a)), q = (int16_t)(16000*sin(a));
        raw[4*k] = i; raw[4*k+1] = (uint16_t)i >> 8;
        raw[4*k+2] = q; raw[4*k+3] = (uint16_t)q >> 8;
    }
    const int navg_before = s->fft.navg;
    const uint32_t sequence_before = s->line.sequence;
    lr_spec_feed(raw, sizeof(raw), pair, gen, ms, spare);
    /* A publication resets navg, so count that transition as its FFT too. */
    if (s->fft.navg > navg_before || s->line.sequence != sequence_before)
        fft_count++;
    pair += sizeof(raw)/4; ms += 3;
}
static void begin(void)
{
    lr_spec_enable(false); lr_spec_end();
    LS_CHECK(lr_spec_enable(true)); lr_spec_begin(433920000);
    pair = 0; ms = 1000; fft_count = 0;
}
static int peak(void)
{
    int best = 0;
    for (int i = 1; i < LR_SPEC_BINS; ++i) if (bins[i] > bins[best]) best = i;
    return best;
}
LS_CASE(signed_iq_tones_land_in_shifted_bins)
{
    const int tones[] = {-80, 0, 64};
    for (unsigned t = 0; t < 3; ++t) {
        begin();
        for (int k = 0; k < 80; ++k) block(tones[t], 1, true);
        LS_CHECK(lr_spec_read(bins, &line, ms));
        LS_EQ_INT((tones[t] + 256)/2, peak());
        LS_NEAR(bins[peak()], 0.938, .003);
        LS_EQ_UINT(433920000, line.center_hz);
    }
}
LS_CASE(one_fft_window_publishes_each_100ms_row)
{
    begin();
    const uint32_t first_sequence = s->line.sequence;
    for (int k = 0; k < 260; ++k) block(64, 1, true);
    const unsigned rows = s->line.sequence - first_sequence;
    LS_CHECK(rows >= 6 && rows <= 8);
    LS_EQ_UINT(rows, fft_count);
    LS_CHECK(lr_spec_read(bins, &line, ms));
    LS_EQ_INT(160, peak());
    printf("LR waterfall FFT budget: %u windows for %u rows in %u ms (prior 20 ms cadence: about 5 windows/row)\n",
           fft_count, rows, ms - 1000);
}
LS_CASE(gaps_reset_partial_frames_and_mark_next_line)
{
    begin();
    for (int k = 0; k < 45; ++k) block(-80, 1, true);
    LS_CHECK(lr_spec_read(bins, &line, ms));
    for (int k = 0; k < 45; ++k) block(-80, 1, true);
    LS_CHECK(lr_spec_read(bins, &line, ms)); LS_CHECK(!line.gap);
    pair += 17;
    for (int k = 0; k < 45; ++k) block(64, 1, true);
    LS_CHECK(lr_spec_read(bins, &line, ms)); LS_CHECK(line.gap);
    LS_EQ_INT(160, peak());
    for (int k = 0; k < 45; ++k) block(64, 2, true);
    LS_CHECK(lr_spec_read(bins, &line, ms)); LS_CHECK(line.gap);
    lr_spec_end(); LS_CHECK(!lr_spec_read(bins, &line, ms));
    lr_spec_begin(433920000); LS_CHECK(!lr_spec_read(bins, &line, ms));
}
LS_CASE(backlog_and_busy_reader_skip_without_waiting_or_allocating)
{
    begin();
    unsigned allocations = ls_shim_heap_call_count();
    for (int k = 0; k < 200; ++k) block(64, 1, false);
    LS_CHECK(!lr_spec_read(bins, &line, ms));
    LS_EQ_INT(0, s->fft.navg);
    atomic_flag_test_and_set(&mailbox);
    for (int k = 0; k < 80; ++k) block(64, 1, true);
    atomic_flag_clear(&mailbox);
    LS_CHECK(!lr_spec_read(bins, &line, ms));
    for (int k = 0; k < 40; ++k) block(64, 1, true);
    LS_CHECK(lr_spec_read(bins, &line, ms)); LS_EQ_INT(160, peak());
    LS_EQ_UINT(allocations, ls_shim_heap_call_count());
    for (int k = 0; k < 40; ++k) block(64, 1, true);
    LS_CHECK(lr_spec_read(bins, &line, ms)); LS_CHECK(!line.gap);
    LS_CHECK(!lr_spec_read(bins, &line, ms + 501));
    lr_spec_enable(false); LS_CHECK(!lr_spec_read(bins, &line, ms));
}

#include "switch_budget.h"
LS_CASE(enable_disable_never_allocates_or_initializes_fft)
{
    /* The owner may already have initialized it; enabling must not touch it. */
    const float before = s->fft.win[17];
    switch_budget_begin();
    for (int i = 0; i < 20; ++i) { lr_spec_enable(true); lr_spec_enable(false); }
    LS_EQ_UINT(switch_budget_end(), 0);
    LS_CHECK(s->fft.win[17] == before);
}
