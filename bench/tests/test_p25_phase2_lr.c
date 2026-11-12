/* Phase II through the LR2021's receive path.
 *
 * The LR2021 hands P25 IQ16 at 125 kHz; p25_lr_frontend lifts it to float
 * IQ at 240 kHz and the same dsp_pipeline demodulates it in Phase II mode
 * (p25_p2_rx_float). This renders 6000-baud pi/4-DQPSK at 240 kHz
 * (lsm_gen), resamples it to the chip's 125 kHz IQ16 at a few signal
 * levels, feeds it in the driver's FIFO-sized blocks and asks for the
 * symbols back.
 */
#include "dsp_pipeline.h"
#include "ls_test.h"
#include "lsm_gen.h"
#include "p25_lr_frontend.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sys_log(unsigned char color, const char *fmt, ...) { (void)color; (void)fmt; }

#define N 4000
#define IN_RATE  (DSP_SAMPLE_RATE * DSP_PRE_DECIM)   /* lsm_gen's rate */
#define LR_RATE  125000
#define TAPS 48

/* u8 IQ at IN_RATE -> IQ16LE at 125 kHz, windowed sinc low-passed at 50 kHz. */
static size_t to_lr_iq16(const uint8_t *iq, size_t pairs, float full_scale, uint8_t *out)
{
    const double ratio = (double)IN_RATE / LR_RATE, cut = 50000.0 / IN_RATE;
    size_t n = 0;
    for (double t = TAPS; t + TAPS < (double)pairs; t += ratio, n++) {
        const long c = (long)floor(t);
        double si = 0, sq = 0, w = 0;
        for (long k = c - TAPS + 1; k <= c + TAPS; k++) {
            const double x = t - (double)k;
            const double win = 0.5 + 0.5 * cos(M_PI * x / TAPS);
            const double h = (fabs(x) < 1e-9 ? 2 * cut : sin(2 * M_PI * cut * x) / (M_PI * x)) * win;
            si += h * (((double)iq[2 * k] - 127.5) / 127.5);
            sq += h * (((double)iq[2 * k + 1] - 127.5) / 127.5);
            w += h;
        }
        const int vi = (int)lround(si / w * full_scale * 32767);
        const int vq = (int)lround(sq / w * full_scale * 32767);
        out[4 * n] = (uint8_t)vi; out[4 * n + 1] = (uint8_t)((unsigned)vi >> 8);
        out[4 * n + 2] = (uint8_t)vq; out[4 * n + 3] = (uint8_t)((unsigned)vq >> 8);
    }
    return n;
}

LS_CASE(lr2021_iq16_at_125k_recovers_phase2_symbols)
{
    enum { IQ_BYTES = N * 2 * (IN_RATE / 6000) };
    int *tx = malloc(N * sizeof(int));
    /* 4 bytes a pair at 125 kHz against 2 at 240 kHz: about 1.05 x IQ_BYTES */
    uint8_t *iq = malloc(IQ_BYTES), *lr = malloc(2 * (size_t)IQ_BYTES);
    float *f = malloc(640 * 2 * sizeof(float));
    LS_CHECK(tx && iq && lr && f);
    if (!tx || !iq || !lr || !f) goto out;
    uint32_t random = 4021;
    static const int levels[] = {1, 3, -1, -3};
    for (int i = 0; i < N; ++i) {
        random = random * 1664525u + 1013904223u;
        tx[i] = levels[random >> 30];
    }
    const struct { float off, noise, scale; } cases[] = {
        {     0, 0.00f, 0.25f },
        {  1200, 0.03f, 0.25f },
        { -1200, 0.03f, 0.25f },
        {   400, 0.03f, 0.02f },   /* weak: ~650 counts of IQ16 */
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        lsm_channel_t ch;
        lsm_channel_clean(&ch);
        ch.freq_off_hz = cases[c].off;
        ch.noise_sd = cases[c].noise;
        ls_rng_t rng;
        ls_rng_seed(&rng, 77 + c);
        LS_EQ_INT(lsm_render_iq(tx, N, &ch, &rng, iq, IQ_BYTES), IQ_BYTES);
        const size_t lr_pairs = to_lr_iq16(iq, IQ_BYTES / 2, cases[c].scale, lr);

        /* p25_p2_rx_float's setup, over p25_lr_generation_begin's C4FM state */
        static dsp_state_t dsp;
        dsp_init(&dsp);
        dsp_set_mode(&dsp, DEMOD_C4FM);
        dsp.phase2 = true;
        dsp_set_gain(&dsp, fabsf(dsp.demod_gain));
        dsp_set_mode(&dsp, DEMOD_CQPSK);
        dsp_reset_cqpsk_loops(&dsp);
        p25_lr_frontend_t fe;
        LS_CHECK(p25_lr_frontend_init(&fe, 0, false));
        static int rx[N + 200];
        int count = 0;
        int16_t audio[8192];
        for (size_t pos = 0; pos < lr_pairs * 4; pos += 1020) {   /* the FIFO's batch */
            size_t bytes = lr_pairs * 4 - pos < 1020 ? lr_pairs * 4 - pos : 1020;
            size_t pairs = p25_lr_frontend_process(&fe, lr + pos, bytes, f, 640);
            LS_CHECK(pairs != SIZE_MAX);
            int n = dsp_process_float_iq(&dsp, f, (int)pairs, audio, 8192);
            for (int j = 0; j + 8 <= n && count < N + 200; j += 8) {
                float v = audio[j] / fabsf(dsp.demod_gain);
                rx[count++] = v >= 0 ? (v > 2 ? 3 : 1) : (v < -2 ? -3 : -1);
            }
        }
        /* Best lag over the whole run, then the same lag's misses before and
           after the frequency loop has pulled in (first 1000 symbols). */
        int best = 0, best_lag = 0, total = N - 400;
        for (int lag = -60; lag <= 60; ++lag) {
            int same = 0;
            for (int i = 200; i < N - 200; ++i)
                if (i + lag >= 0 && i + lag < count && rx[i + lag] == tx[i]) ++same;
            if (same > best) { best = same; best_lag = lag; }
        }
        int early = 0, late = 0;
        for (int i = 200; i < N - 200; ++i) {
            const int j = i + best_lag;
            if (j < 0 || j >= count || rx[j] != tx[i]) ++*(i < 1000 ? &early : &late);
        }
        ls_note("LR 125k IQ16 offset %+.0f Hz scale %.2f: recovered %d, matched %d/%d "
                "(misses %d in pull-in, %d after)", (double)cases[c].off,
                (double)cases[c].scale, count, best, total, early, late);
        LS_CHECK(abs(count - N) < 60);
        LS_CHECK(best > total * 97 / 100);
        LS_CHECK(late * 1000 < (N - 1200) * 5);   /* under 0.5% once locked */
    }
out:
    free(tx); free(iq); free(lr); free(f);
}
