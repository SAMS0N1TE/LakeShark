

#include "ls_test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "dsp_pipeline.h"

/* dsp_pipeline.c logs its own diagnostics; the bench does not want them. */
void sys_log(unsigned char color, const char *fmt, ...) { (void)color; (void)fmt; }

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BAUD            4800
#define IQ_RATE         DSP_SAMPLE_RATE            /* 240 kHz */
#define SPS_IQ          (IQ_RATE / BAUD)           /* 50 IQ pairs per symbol */
#define SIGNAL_SYMBOLS  (BAUD)                     /* one second of signal */
#define SIGNAL_PAIRS    (SIGNAL_SYMBOLS * SPS_IQ)  /* 240000 IQ pairs */
#define BLOCK_PAIRS     (16384 / 2)                /* P25_IQ_BLOCK_BYTES on device */

/* Deviation of the four C4FM symbols, in Hz. */
static const double SYM_DEV_HZ[4] = { 1800.0, 600.0, -600.0, -1800.0 };

/*
 * One second of 4-level FM at 4800 baud, rendered as u8 interleaved IQ at
 * 240 kHz - the same shape and the same format the RTL-SDR hands the P25
 * app. Symbol values come from ls_rng so the run is identical everywhere.
 * No pulse shaping: this case measures work per sample, and the filter
 * chain does the same work whatever the pulse looks like.
 */
static void make_c4fm_iq(uint8_t *iq, int pairs, unsigned long long seed)
{
    ls_rng_t rng;
    ls_rng_seed(&rng, seed);
    double phase = 0.0;
    double dev = 0.0;
    for (int k = 0; k < pairs; k++) {
        if (k % SPS_IQ == 0) dev = SYM_DEV_HZ[ls_rng_u32(&rng) & 3];
        phase += 2.0 * M_PI * dev / (double)IQ_RATE;
        if (phase >  M_PI) phase -= 2.0 * M_PI;
        if (phase < -M_PI) phase += 2.0 * M_PI;
        double i = 0.7 * cos(phase);
        double q = 0.7 * sin(phase);
        iq[k * 2]     = (uint8_t)(127.5 + i * 127.0);
        iq[k * 2 + 1] = (uint8_t)(127.5 + q * 127.0);
    }
}

/*
 * Push the signal through one mode in device-sized blocks, REPS times over
 * the same dsp_state so the stream stays continuous. REPS exists only to
 * beat clock()'s granularity: one second of signal takes single-digit
 * milliseconds here, and CLOCKS_PER_SEC on Windows quantises to about that,
 * so a single pass reports 4 ms or 7 ms depending on where the tick fell.
 *
 * Returns the samples produced by the LAST pass (so the caller can check
 * the rate against one second of input); *ms_out is the per-pass host time.
 */
#define REPS 16

static int run_mode(demod_mode_t mode, const uint8_t *iq, int pairs,
                    int16_t *out, int out_cap, double *ms_out)
{
    dsp_state_t *s = calloc(1, sizeof(*s));
    LS_CHECK(s != NULL);
    if (!s) { *ms_out = 0; return 0; }
    dsp_init(s);
    dsp_set_mode(s, mode);

    int n = 0;
    clock_t t0 = clock();
    for (int rep = 0; rep < REPS; rep++) {
        n = 0;
        for (int off = 0; off < pairs; off += BLOCK_PAIRS) {
            int want = pairs - off;
            if (want > BLOCK_PAIRS) want = BLOCK_PAIRS;
            n += dsp_process_iq(s, iq + (size_t)off * 2, want * 2,
                                out + n, out_cap - n);
        }
    }
    clock_t t1 = clock();
    *ms_out = 1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC / (double)REPS;
    free(s);
    return n;
}

/*
 * The decimator must not lose its phase at a block boundary. 240000 pairs
 * in, DSP_DECIMATION of 5, so 48000 samples out - not 47999, not 48029.
 * This is the assertion the case actually stands on.
 */
LS_CASE(c4fm_decimation_is_exact_across_blocks)
{
    uint8_t *iq = malloc((size_t)SIGNAL_PAIRS * 2);
    int16_t *out = malloc(sizeof(int16_t) * SIGNAL_PAIRS);
    LS_CHECK(iq != NULL && out != NULL);
    if (!iq || !out) { free(iq); free(out); return; }

    make_c4fm_iq(iq, SIGNAL_PAIRS, 0xC4F0C057ull);
    double ms = 0;
    int n = run_mode(DEMOD_C4FM, iq, SIGNAL_PAIRS, out, SIGNAL_PAIRS, &ms);

    LS_EQ_INT(n, SIGNAL_PAIRS / DSP_DECIMATION);
    ls_note("C4FM: %d pairs -> %d samples in %.1f ms host "
            "(%.1f ns/pair, %.2f%% of one host core per second of signal)",
            SIGNAL_PAIRS, n, ms,
            ms * 1e6 / (double)SIGNAL_PAIRS, ms / 10.0);

    free(iq);
    free(out);
}

/*
 * Relative cost of the four modes over identical input. The absolute
 * numbers mean nothing off this machine; the ratios are what 656's Phase 2
 * estimate is built on, and what a future H-DQPSK front end gets compared
 * against. Every mode must produce output - a mode that silently emits
 * nothing would otherwise look like the cheapest one.
 */
LS_CASE(all_modes_produce_output_and_report_cost)
{
    static const char *NAMES[4] = { "C4FM", "CQPSK", "DIFF_4FSK", "FSK4_TRACKING" };
    uint8_t *iq = malloc((size_t)SIGNAL_PAIRS * 2);
    int16_t *out = malloc(sizeof(int16_t) * SIGNAL_PAIRS);
    LS_CHECK(iq != NULL && out != NULL);
    if (!iq || !out) { free(iq); free(out); return; }

    make_c4fm_iq(iq, SIGNAL_PAIRS, 0xC4F0C057ull);

    double base_ms = 0;
    for (int m = 0; m <= (int)DEMOD_FSK4_TRACKING; m++) {
        double ms = 0;
        int n = run_mode((demod_mode_t)m, iq, SIGNAL_PAIRS, out, SIGNAL_PAIRS, &ms);
        if (m == (int)DEMOD_C4FM) base_ms = ms;

        LS_CHECK_MSG(n > 0, "%s produced no output samples", NAMES[m]);
        LS_CHECK_MSG(n > SIGNAL_PAIRS / DSP_DECIMATION * 9 / 10 &&
                     n < SIGNAL_PAIRS / DSP_DECIMATION * 11 / 10,
                     "%s produced %d samples for %d input pairs; every mode "
                     "must hand DSD roughly one sample per decimated input "
                     "(%d) because the slicer is configured for DSP_SPS "
                     "samples per symbol at that rate",
                     NAMES[m], n, SIGNAL_PAIRS, SIGNAL_PAIRS / DSP_DECIMATION);

        ls_note("%-14s %6d out  %7.1f ms host  %5.2fx C4FM",
                NAMES[m], n, ms, base_ms > 0 ? ms / base_ms : 0.0);
    }

    free(iq);
    free(out);
}
