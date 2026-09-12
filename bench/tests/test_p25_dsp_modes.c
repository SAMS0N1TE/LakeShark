

#include "ls_test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dsp_pipeline.h"

/* dsp_pipeline.c calls sys_log() for its own diagnostics. On device that
 * routes to the event log. Here we swallow it - the tests inspect output
 * samples, not log text. */
void sys_log(unsigned char color, const char *fmt, ...) { (void)color; (void)fmt; }

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BAUD              4800
#define IQ_RATE           DSP_SAMPLE_RATE          /* 240 kHz */
#define SPS_IQ            (IQ_RATE / BAUD)         /* 50 samples per symbol at 240 kHz */
#define P25_DEV_INNER_HZ  600.0
#define P25_DEV_OUTER_HZ  1800.0

/*
 * Half-cosine transition between adjacent symbol targets. Not a true RRC
 * pulse but close enough that the dsp_pipeline LPF sees a bandlimited
 * signal instead of hard steps that would ring through the whole chain.
 * Real transmitters use RRC, but for a "does it decode at all" check the
 * exact pulse shape does not matter.
 */
static double smooth_ramp(double a, double b, int i, int n)
{
    double u = (double)i / (double)n;                 /* 0..1 */
    double w = 0.5 - 0.5 * cos(M_PI * u);             /* half cosine 0..1 */
    return a + (b - a) * w;
}

/*
 * P25 C4FM: symbol value s in {-3,-1,+1,+3} maps to instantaneous frequency
 * deviation s * 600 Hz. Baseband IQ = exp(j * phi) where phi accumulates
 * 2*pi * f_hz / IQ_RATE per sample. Output packed as uint8_t I/Q pairs
 * centered at 127.5 (RTL-SDR convention, which dsp_process_iq expects).
 */
static void gen_c4fm_iq(const int8_t *symbols, int n_symbols,
                        uint8_t *iq_out /* size = 2 * n_symbols * SPS_IQ */)
{
    double phi = 0.0;
    int last = 0;
    for (int s = 0; s < n_symbols; s++) {
        int cur = symbols[s];
        for (int k = 0; k < SPS_IQ; k++) {
            /* smooth from last to cur over ~half the symbol; hold the rest */
            double sym;
            int half = SPS_IQ / 2;
            if (k < half) sym = smooth_ramp((double)last, (double)cur, k, half);
            else          sym = (double)cur;

            double f_hz = sym * P25_DEV_INNER_HZ;    /* +/-1 -> +/-600, +/-3 -> +/-1800 */
            phi += 2.0 * M_PI * f_hz / (double)IQ_RATE;
            /* wrap to keep magnitudes stable */
            if (phi >  2.0 * M_PI) phi -= 2.0 * M_PI;
            if (phi < -2.0 * M_PI) phi += 2.0 * M_PI;

            double i = 0.7 * cos(phi);
            double q = 0.7 * sin(phi);
            int idx = (s * SPS_IQ + k) * 2;
            int ii = (int)(127.5 + i * 120.0);
            int qq = (int)(127.5 + q * 120.0);
            if (ii < 0) ii = 0;
            if (ii > 255) ii = 255;
            if (qq < 0) qq = 0;
            if (qq > 255) qq = 255;
            iq_out[idx    ] = (uint8_t)ii;
            iq_out[idx + 1] = (uint8_t)qq;
        }
        last = cur;
    }
}

/*
 * pi/4-DQPSK: dibit d in {0,1,2,3} maps to differential phase step in
 * {+pi/4, +3pi/4, -3pi/4, -pi/4}. Each symbol phase held for SPS_IQ IQ
 * samples with half-cosine transition. This is the signal the CQPSK path
 * (Gardner + diff phasor + residual AFC + atan2 rescale) is designed for.
 */
static void gen_dqpsk_iq(const int8_t *dibits, int n_symbols,
                         uint8_t *iq_out /* size = 2 * n_symbols * SPS_IQ */)
{
    static const double DPH[4] = {  M_PI / 4.0,  3.0 * M_PI / 4.0,
                                   -M_PI / 4.0, -3.0 * M_PI / 4.0 };
    double phi_cur  = 0.0;
    double phi_prev = 0.0;
    for (int s = 0; s < n_symbols; s++) {
        int d = dibits[s] & 3;
        phi_cur = phi_prev + DPH[d];
        while (phi_cur >  2.0 * M_PI) phi_cur -= 2.0 * M_PI;
        while (phi_cur < -2.0 * M_PI) phi_cur += 2.0 * M_PI;

        for (int k = 0; k < SPS_IQ; k++) {
            double phi;
            int half = SPS_IQ / 2;
            if (k < half) phi = smooth_ramp(phi_prev, phi_cur, k, half);
            else          phi = phi_cur;

            double i = 0.7 * cos(phi);
            double q = 0.7 * sin(phi);
            int idx = (s * SPS_IQ + k) * 2;
            int ii = (int)(127.5 + i * 120.0);
            int qq = (int)(127.5 + q * 120.0);
            if (ii < 0) ii = 0;
            if (ii > 255) ii = 255;
            if (qq < 0) qq = 0;
            if (qq > 255) qq = 255;
            iq_out[idx    ] = (uint8_t)ii;
            iq_out[idx + 1] = (uint8_t)qq;
        }
        phi_prev = phi_cur;
    }
}

/*
 * Build a balanced repeating symbol sequence across all 4 levels so the
 * output distribution ought to show four equally-populated clusters. A
 * uniform sequence is enough for a "does it work" check - random sequences
 * carry statistical noise that makes cluster tests flaky.
 */
static void fill_c4fm_pattern(int8_t *symbols, int n)
{
    static const int8_t seq[] = { +3, +1, -1, -3, +1, +3, -3, -1,
                                  -1, -3, +3, +1, -3, -1, +1, +3 };
    for (int i = 0; i < n; i++) symbols[i] = seq[i & 15];
}
static void fill_dqpsk_pattern(int8_t *dibits, int n)
{
    static const int8_t seq[] = { 0, 1, 2, 3, 1, 2, 3, 0,
                                  2, 3, 0, 1, 3, 0, 1, 2 };
    for (int i = 0; i < n; i++) dibits[i] = seq[i & 15];
}

/*
 * Cluster metric. Skips a settling window at the front, then partitions
 * the remainder by quartiles. Reports the four cluster means; the caller
 * decides what "separated enough" means for its mode.
 *
 * This is not k-means; it does not need to be. The input pattern gives us
 * a balanced four-level output, so the four quartiles of the histogram are
 * a sound estimator of the cluster centers.
 */
typedef struct {
    double c_neg_outer;
    double c_neg_inner;
    double c_pos_inner;
    double c_pos_outer;
    int    n;
} four_level_stats_t;

static int cmp_int16(const void *a, const void *b)
{
    int16_t A = *(const int16_t *)a, B = *(const int16_t *)b;
    return (A > B) - (A < B);
}

static four_level_stats_t analyse_output(const int16_t *audio, int n_total,
                                         int settle_samples)
{
    four_level_stats_t r = {0};
    int start = settle_samples;
    if (start >= n_total) start = n_total / 2;
    int n = n_total - start;
    if (n < 100) return r;

    int16_t *buf = malloc(sizeof(int16_t) * n);
    memcpy(buf, audio + start, sizeof(int16_t) * n);
    qsort(buf, n, sizeof(int16_t), cmp_int16);

    /* Quartile-boundaries */
    int q1 = n / 4;
    int q2 = n / 2;
    int q3 = (3 * n) / 4;

    /* Cluster means (skip 5% at the extreme edges to avoid a single
     * outlier from dragging the mean). */
    int trim = n / 20;
    if (trim < 1) trim = 1;

    double s = 0; int c = 0;
    for (int i = trim; i < q1; i++)      { s += buf[i]; c++; }
    r.c_neg_outer = c ? s / c : 0;

    s = 0; c = 0;
    for (int i = q1; i < q2; i++)        { s += buf[i]; c++; }
    r.c_neg_inner = c ? s / c : 0;

    s = 0; c = 0;
    for (int i = q2; i < q3; i++)        { s += buf[i]; c++; }
    r.c_pos_inner = c ? s / c : 0;

    s = 0; c = 0;
    for (int i = q3; i < n - trim; i++)  { s += buf[i]; c++; }
    r.c_pos_outer = c ? s / c : 0;

    r.n = n;
    free(buf);
    return r;
}

/*
 * The four-level check itself. Order-independent because polarity can be
 * either sign - what matters is that we have four distinct clusters, two
 * negative and two positive, with the outer bigger than the inner on each
 * side.
 */
static void check_four_levels(const char *label, four_level_stats_t st,
                              double min_inner_mag, double min_outer_mag)
{
    ls_note("%s: neg_outer=%.0f neg_inner=%.0f pos_inner=%.0f pos_outer=%.0f (n=%d)",
            label, st.c_neg_outer, st.c_neg_inner,
            st.c_pos_inner, st.c_pos_outer, st.n);

    LS_CHECK_MSG(st.c_neg_outer < 0, "%s: neg_outer not negative", label);
    LS_CHECK_MSG(st.c_neg_inner < 0, "%s: neg_inner not negative", label);
    LS_CHECK_MSG(st.c_pos_inner > 0, "%s: pos_inner not positive", label);
    LS_CHECK_MSG(st.c_pos_outer > 0, "%s: pos_outer not positive", label);
    LS_CHECK_MSG(st.c_neg_outer < st.c_neg_inner,
                 "%s: neg_outer not more negative than neg_inner", label);
    LS_CHECK_MSG(st.c_pos_outer > st.c_pos_inner,
                 "%s: pos_outer not more positive than pos_inner", label);
    LS_CHECK_MSG(fabs(st.c_pos_inner) >= min_inner_mag &&
                 fabs(st.c_neg_inner) >= min_inner_mag,
                 "%s: inner cluster magnitude below threshold", label);
    LS_CHECK_MSG(fabs(st.c_pos_outer) >= min_outer_mag &&
                 fabs(st.c_neg_outer) >= min_outer_mag,
                 "%s: outer cluster magnitude below threshold", label);
}

/* Convenience: run enough IQ through a mode to fill a big output buffer,
 * then hand back the cluster stats and let the caller judge. */
static four_level_stats_t run_mode(demod_mode_t mode,
                                   const uint8_t *iq, int iq_bytes,
                                   int settle_symbols)
{
    dsp_state_t s;
    dsp_init(&s);
    dsp_set_mode(&s, mode);
    /* Use unit gain so cluster magnitudes reflect the pipeline's own scaling. */
    dsp_set_gain(&s, 9000.0f);

    /* Output rate = IQ_RATE / DSP_DECIMATION (for C4FM/DIFF_4FSK/FSK4_TRACKING
     * post-decimation samples are emitted 1:1) or for CQPSK repeated at DSP_SPS
     * per symbol. Generous cap covers all modes. */
    int max_out = iq_bytes;   /* wildly more than needed; audio can't exceed IQ pairs */
    int16_t *audio = calloc(max_out, sizeof(int16_t));

    /* Push in 4 kB chunks - the same shape p25_rx_task uses. */
    int n_out = 0;
    int off   = 0;
    while (off < iq_bytes && n_out < max_out) {
        int chunk = iq_bytes - off;
        if (chunk > 4096) chunk = 4096;
        int got = dsp_process_iq(&s, iq + off, chunk, audio + n_out, max_out - n_out);
        n_out += got;
        off   += chunk;
    }

    /* Settling window: assume DSP_AUDIO_RATE per second of audio; give a mode
     * (settle_symbols / BAUD) seconds worth. For CQPSK and DIFF_4FSK the
     * per-symbol output count can be > 1, so overshoot on the settle count. */
    int settle_samples = settle_symbols * (DSP_AUDIO_RATE / BAUD);
    four_level_stats_t st = analyse_output(audio, n_out, settle_samples);
    free(audio);
    return st;
}

/* the disabled DIFF_4FSK I/Q RRC left two 51-float delay lines in
 * every dsp_state_t even though no sample ever entered them. Keep the state
 * below the post-removal ceiling so dormant per-mode buffers cannot silently
 * consume another 412 bytes of the production P25 state allocation. */
LS_CASE(dsp_state_excludes_dormant_diff_4fsk_rrc_buffers)
{
    LS_CHECK_MSG(sizeof(dsp_state_t) <= 1040,
                 "dsp_state_t is %zu bytes; expected at most 1040",
                 sizeof(dsp_state_t));
}

/* --- C4FM waveform, C4FM demod: the currently-working path. If this ever
 *     breaks we ship dead radio, so it is the strictest test. */
LS_CASE(c4fm_waveform_through_c4fm_mode)
{
    const int N = 4000;
    int8_t *syms = malloc(N);
    fill_c4fm_pattern(syms, N);

    int iq_bytes = 2 * N * SPS_IQ;
    uint8_t *iq = malloc(iq_bytes);
    gen_c4fm_iq(syms, N, iq);

    four_level_stats_t st = run_mode(DEMOD_C4FM, iq, iq_bytes, /* settle=*/1500);
    check_four_levels("C4FM<-C4FM", st, /* inner=*/500.0, /* outer=*/1500.0);

    free(iq); free(syms);
}

/* --- C4FM waveform, FSK4_TRACKING demod: OP25 3-loop tracker. Same input
 *     kind, different back end. Must also produce 4 levels. */
LS_CASE(c4fm_waveform_through_fsk4_tracking_mode)
{
    const int N = 4000;
    int8_t *syms = malloc(N);
    fill_c4fm_pattern(syms, N);

    int iq_bytes = 2 * N * SPS_IQ;
    uint8_t *iq = malloc(iq_bytes);
    gen_c4fm_iq(syms, N, iq);

    four_level_stats_t st = run_mode(DEMOD_FSK4_TRACKING, iq, iq_bytes,
                                     /* settle=*/2000);
    check_four_levels("FSK4T<-C4FM", st, /* inner=*/1000.0, /* outer=*/3000.0);

    free(iq); free(syms);
}

/* --- C4FM waveform, DIFF_4FSK demod: SDRTrunk-style differential
 *     demodulation. */
LS_CASE(c4fm_waveform_through_diff_4fsk_mode)
{
    const int N = 4000;
    int8_t *syms = malloc(N);
    fill_c4fm_pattern(syms, N);

    int iq_bytes = 2 * N * SPS_IQ;
    uint8_t *iq = malloc(iq_bytes);
    gen_c4fm_iq(syms, N, iq);

    four_level_stats_t st = run_mode(DEMOD_DIFF_4FSK, iq, iq_bytes,
                                     /* settle=*/1500);
    check_four_levels("DIFF<-C4FM", st, /* inner=*/500.0, /* outer=*/1500.0);

    free(iq); free(syms);
}

/* --- DQPSK waveform, CQPSK demod: Gardner + diff phasor + residual AFC.
 *     This is what LSM (simulcast) fundamentally looks like. */
LS_CASE(dqpsk_waveform_through_cqpsk_mode)
{
    const int N = 4000;
    int8_t *dibits = malloc(N);
    fill_dqpsk_pattern(dibits, N);

    int iq_bytes = 2 * N * SPS_IQ;
    uint8_t *iq = malloc(iq_bytes);
    gen_dqpsk_iq(dibits, N, iq);

    four_level_stats_t st = run_mode(DEMOD_CQPSK, iq, iq_bytes,
                                     /* settle=*/2500);
    check_four_levels("CQPSK<-DQPSK", st, /* inner=*/500.0, /* outer=*/1500.0);

    free(iq); free(dibits);
}

/*
 * Mode index clamp. p25_demod_mode_clamp is the single helper that turns
 * a stored (or wire-received) integer into a valid demod_mode_t, so
 * settings NVS corruption or a stale Flipper-link value cannot land in
 * dsp_set_mode() as an out-of-range enum. If someone widens demod_mode_t,
 * this test fails until the helper is updated too.
 */
#include "p25_demod_mode.h"

LS_CASE(mode_index_clamp_valid_passthrough)
{
    LS_EQ_INT(p25_demod_mode_clamp(0, DEMOD_C4FM), DEMOD_C4FM);
    LS_EQ_INT(p25_demod_mode_clamp(1, DEMOD_C4FM), DEMOD_CQPSK);
    LS_EQ_INT(p25_demod_mode_clamp(2, DEMOD_C4FM), DEMOD_DIFF_4FSK);
    LS_EQ_INT(p25_demod_mode_clamp(3, DEMOD_C4FM), DEMOD_FSK4_TRACKING);
}

LS_CASE(mode_index_clamp_out_of_range_uses_default)
{
    LS_EQ_INT(p25_demod_mode_clamp(-1,  DEMOD_C4FM),   DEMOD_C4FM);
    LS_EQ_INT(p25_demod_mode_clamp( 4,  DEMOD_CQPSK),  DEMOD_CQPSK);
    LS_EQ_INT(p25_demod_mode_clamp( 99, DEMOD_C4FM),   DEMOD_C4FM);
    /* A bogus default also gets clamped, so a corrupted call site cannot
     * poison a mode read. */
    LS_EQ_INT(p25_demod_mode_clamp( 0,  (demod_mode_t)77), DEMOD_C4FM);
}
