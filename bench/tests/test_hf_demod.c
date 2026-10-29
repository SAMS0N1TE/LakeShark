/* HF demodulators on synthesized IQ: SSB tone recovery and opposite-sideband
   rejection, AM and synchronous AM recovery, CW pitch and selectivity, the
   clarifier, the blanker, and how fast the AGC settles. */
#include "ls_test.h"
#include "hf_demod.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI2 6.283185307179586

static hf_demod_t dsp;
static float iq[2 * HF_IN_RATE];          /* one second of input */
static uint8_t iq8[2 * HF_IN_RATE];
static int16_t pcm[HF_OUT_RATE + 64];

/* Up to two tones and an AM carrier, at frequencies relative to the IQ centre. */
typedef struct {
    double f1, a1, f2, a2;   /* plain complex tones */
    double am_f, am_a, am_m, am_mod_hz;
} sig_t;

static void make(const sig_t *g, double t0, size_t pairs)
{
    for (size_t k = 0; k < pairs; ++k) {
        const double t = t0 + (double)k / HF_IN_RATE;
        double i = 0, q = 0;
        if (g->a1) { i += g->a1 * cos(PI2 * g->f1 * t); q += g->a1 * sin(PI2 * g->f1 * t); }
        if (g->a2) { i += g->a2 * cos(PI2 * g->f2 * t); q += g->a2 * sin(PI2 * g->f2 * t); }
        if (g->am_a) {
            const double env = g->am_a * (1.0 + g->am_m * cos(PI2 * g->am_mod_hz * t));
            i += env * cos(PI2 * g->am_f * t);
            q += env * sin(PI2 * g->am_f * t);
        }
        iq[2 * k] = (float)i;
        iq[2 * k + 1] = (float)q;
    }
}

/* Amplitude of the component at `hz` in the PCM (Goertzel, normalized). */
static double tone_amp(const int16_t *x, int n, double hz)
{
    const double w = PI2 * hz / HF_OUT_RATE, c = 2 * cos(w);
    double s1 = 0, s2 = 0;
    for (int k = 0; k < n; ++k) {
        const double s0 = x[k] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * sqrt(p > 0 ? p : 0) / n;
}

static double rms(const int16_t *x, int n)
{
    double s = 0;
    for (int k = 0; k < n; ++k) s += (double)x[k] * x[k];
    return n ? sqrt(s / n) : 0;
}

static hf_demod_cfg_t cfg_for(hf_demod_mode_t mode, uint32_t width, hf_agc_t agc)
{
    hf_demod_cfg_t c = { .mode = mode, .filter_hz = width, .tune_hz = 20000,
                         .rit_hz = 0, .cw_pitch_hz = 700, .agc = agc, .nb = HF_NB_OFF };
    return c;
}

/* Run `seconds` of signal; returns PCM samples from the last second. */
static int run(const sig_t *g, double seconds)
{
    int n = 0;
    for (double t = 0; t < seconds - 1e-9; t += 1.0) {
        make(g, t, HF_IN_RATE);
        n = hf_demod_cf32(&dsp, iq, HF_IN_RATE, pcm, HF_OUT_RATE + 64);
    }
    return n;
}

LS_CASE(usb_recovers_a_tone_and_lsb_rejects_it)
{
    /* Dial at +20 kHz in the IQ; a USB voice tone 1 kHz above it. */
    /* AGC off holds the starting 30 dB, so keep the tone below the limiter. */
    const sig_t g = { .f1 = 21000, .a1 = 0.004 };
    hf_demod_init(&dsp);
    hf_demod_cfg_t c = cfg_for(HF_DEMOD_USB, 2400, HF_AGC_OFF);
    hf_demod_set(&dsp, &c);
    int n = run(&g, 1.0);
    LS_EQ_INT(HF_OUT_RATE, n);
    const double usb = tone_amp(pcm, n, 1000);
    LS_CHECK_MSG(usb > 300, "USB tone %.1f", usb);
    /* Nearly all the output is that tone. */
    LS_CHECK(usb / sqrt(2.0) > 0.95 * rms(pcm, n));

    hf_demod_init(&dsp);
    c.mode = HF_DEMOD_LSB;
    hf_demod_set(&dsp, &c);
    n = run(&g, 1.0);
    /* Skip the first 100 ms: a carrier switched on from nothing splashes. */
    const double lsb = rms(pcm + 1600, n - 1600);
    const double rej_db = 20 * log10(usb / sqrt(2.0) / (lsb + 1e-3));
    LS_CHECK_MSG(rej_db > 50.0, "opposite sideband only %.1f dB down", rej_db);
}

LS_CASE(lsb_recovers_a_tone_below_the_carrier)
{
    const sig_t g = { .f1 = 20000 - 1500, .a1 = 0.004 };
    hf_demod_init(&dsp);
    hf_demod_cfg_t c = cfg_for(HF_DEMOD_LSB, 2800, HF_AGC_OFF);
    hf_demod_set(&dsp, &c);
    const int n = run(&g, 1.0);
    const double a = tone_amp(pcm, n, 1500);
    LS_CHECK_MSG(a > 300, "LSB tone %.1f", a);
    LS_CHECK(a / sqrt(2.0) > 0.95 * rms(pcm, n));
    /* USB on the same signal hears nothing of it. */
    hf_demod_init(&dsp);
    c.mode = HF_DEMOD_USB;
    hf_demod_set(&dsp, &c);
    const int m = run(&g, 1.0);
    LS_CHECK(20 * log10(a / sqrt(2.0) / (rms(pcm + 1600, m - 1600) + 1e-3)) > 50.0);
}

LS_CASE(am_and_sam_recover_the_modulation)
{
    const sig_t g = { .am_f = 20000, .am_a = 0.05, .am_m = 0.5, .am_mod_hz = 800 };
    for (int mode = HF_DEMOD_AM; mode <= HF_DEMOD_SAM; ++mode) {
        hf_demod_init(&dsp);
        hf_demod_cfg_t c = cfg_for((hf_demod_mode_t)mode, 6000, HF_AGC_SLOW);
        hf_demod_set(&dsp, &c);
        const int n = run(&g, 2.0);
        const double a = tone_amp(pcm, n, 800);
        LS_CHECK_MSG(a > 1000, "mode %d tone %.1f", mode, a);
        /* The carrier itself is gone: what is left is the 800 Hz tone. */
        LS_CHECK_MSG(a / sqrt(2.0) > 0.9 * rms(pcm, n), "mode %d not clean", mode);
        LS_CHECK(abs(pcm[n / 2]) <= HF_PCM_LIMIT);
    }
}

LS_CASE(sam_locks_to_a_carrier_off_the_dial)
{
    /* 40 Hz of mistuning: SAM pulls in and still gives clean audio. */
    const sig_t g = { .am_f = 20040, .am_a = 0.05, .am_m = 0.4, .am_mod_hz = 1000 };
    hf_demod_init(&dsp);
    hf_demod_cfg_t c = cfg_for(HF_DEMOD_SAM, 8000, HF_AGC_SLOW);
    hf_demod_set(&dsp, &c);
    const int n = run(&g, 2.0);
    LS_NEAR(40.0, hf_demod_sam_offset_hz(&dsp), 2.0);
    const double a = tone_amp(pcm, n, 1000);
    LS_CHECK(a / sqrt(2.0) > 0.9 * rms(pcm, n));
}

LS_CASE(cw_is_heard_at_the_pitch_and_a_neighbour_is_not)
{
    const sig_t on = { .f1 = 20000, .a1 = 0.003 };
    hf_demod_init(&dsp);
    hf_demod_cfg_t c = cfg_for(HF_DEMOD_CW, 250, HF_AGC_OFF);
    hf_demod_set(&dsp, &c);
    int n = run(&on, 1.0);
    const double a = tone_amp(pcm, n, 700);
    LS_CHECK_MSG(a > 100, "CW tone %.1f", a);
    /* A carrier 400 Hz away, 10x stronger, is 60+ dB down. */
    const sig_t off = { .f1 = 20400, .a1 = 0.03 };
    hf_demod_init(&dsp);
    hf_demod_set(&dsp, &c);
    n = run(&off, 1.0);
    const double nb = rms(pcm + 1600, n - 1600);
    LS_CHECK_MSG(20 * log10(a / sqrt(2.0) / (nb + 1e-3)) > 60.0 - 20.0,
                 "neighbour %.1f vs %.1f", nb, a);
}

LS_CASE(the_clarifier_moves_what_is_heard)
{
    const sig_t g = { .f1 = 21000, .a1 = 0.02 };
    hf_demod_init(&dsp);
    hf_demod_cfg_t c = cfg_for(HF_DEMOD_USB, 2400, HF_AGC_OFF);
    c.rit_hz = 100;
    hf_demod_set(&dsp, &c);
    const int n = run(&g, 1.0);
    LS_CHECK(tone_amp(pcm, n, 900) > 10 * tone_amp(pcm, n, 1000));
    LS_CHECK(tone_amp(pcm, n, 900) > 300);
    c.rit_hz = -50;
    hf_demod_set(&dsp, &c);
    const int m = run(&g, 1.0);
    LS_CHECK(tone_amp(pcm, m, 1050) > 10 * tone_amp(pcm, m, 1000));
}

LS_CASE(agc_settles_both_ways_without_blasting)
{
    hf_demod_init(&dsp);
    hf_demod_cfg_t c = cfg_for(HF_DEMOD_USB, 2400, HF_AGC_FAST);
    hf_demod_set(&dsp, &c);
    /* Loud, then 40 dB quieter, then loud again. */
    sig_t g = { .f1 = 21000, .a1 = 0.3 };
    int n = run(&g, 1.0);
    LS_NEAR(HF_PCM_TARGET, tone_amp(pcm, n, 1000), HF_PCM_TARGET * 0.1);

    g.a1 = 0.003;
    n = run(&g, 1.0);
    /* 40 dB at 30 dB/s after an 80 ms hang: 1.4 s. The last half of the
       second second is settled. */
    n = run(&g, 1.0);
    LS_NEAR(HF_PCM_TARGET, tone_amp(pcm + n / 2, n / 2, 1000), HF_PCM_TARGET * 0.1);

    /* The jump back up is caught at once: no run of clipped samples. */
    g.a1 = 0.3;
    make(&g, 0, HF_IN_RATE);
    n = hf_demod_cf32(&dsp, iq, HF_IN_RATE, pcm, HF_OUT_RATE + 64);
    int clipped = 0;
    for (int k = 0; k < n; ++k) clipped += abs(pcm[k]) >= HF_PCM_LIMIT;
    LS_CHECK_MSG(clipped < n / 200, "%d clipped", clipped);
    LS_NEAR(HF_PCM_TARGET, tone_amp(pcm + n / 10, n - n / 10, 1000), HF_PCM_TARGET * 0.1);
}

LS_CASE(the_blanker_removes_impulse_noise)
{
    /* A weak USB tone with a sharp impulse every 5 ms, 50x its amplitude. */
    const sig_t g = { .f1 = 21000, .a1 = 0.002 };
    double residual[2];
    for (int nb = 0; nb < 2; ++nb) {
        hf_demod_init(&dsp);
        hf_demod_cfg_t c = cfg_for(HF_DEMOD_USB, 2400, HF_AGC_OFF);
        c.nb = nb ? HF_NB_HIGH : HF_NB_OFF;
        hf_demod_set(&dsp, &c);
        make(&g, 0, HF_IN_RATE);
        for (size_t k = 0; k < HF_IN_RATE; k += HF_IN_RATE / 200)
            for (int j = 0; j < 4; ++j) { iq[2 * (k + j)] += 0.1f; iq[2 * (k + j) + 1] -= 0.1f; }
        const int n = hf_demod_cf32(&dsp, iq, HF_IN_RATE, pcm, HF_OUT_RATE + 64);
        const double t = tone_amp(pcm + 1600, n - 1600, 1000) / sqrt(2.0);
        const double r = rms(pcm + 1600, n - 1600);
        residual[nb] = sqrt(fmax(r * r - t * t, 0));
    }
    LS_CHECK_MSG(residual[1] * 4 < residual[0], "noise %.1f -> %.1f", residual[0], residual[1]);
    LS_CHECK(hf_demod_blanked(&dsp) > 0);
}

LS_CASE(eight_bit_iq_from_a_dongle_works_the_same)
{
    hf_demod_init(&dsp);
    hf_demod_cfg_t c = cfg_for(HF_DEMOD_USB, 2400, HF_AGC_SLOW);
    hf_demod_set(&dsp, &c);
    const sig_t g = { .f1 = 21200, .a1 = 0.25 };
    int n = 0;
    for (int s = 0; s < 2; ++s) {
        make(&g, s, HF_IN_RATE);
        for (size_t k = 0; k < 2 * (size_t)HF_IN_RATE; ++k) {
            const long v = lrintf(iq[k] * 128.0f + 127.5f);
            iq8[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        /* In odd-sized pieces, as reads arrive. */
        n = 0;
        for (size_t off = 0; off < sizeof(iq8); off += 6002) {
            const size_t len = sizeof(iq8) - off < 6002 ? sizeof(iq8) - off : 6002;
            n += hf_demod_u8(&dsp, iq8 + off, len, pcm + n, HF_OUT_RATE + 64 - n);
        }
    }
    LS_EQ_INT(HF_OUT_RATE, n);
    const double a = tone_amp(pcm, n, 1200);
    LS_NEAR(HF_PCM_TARGET, a, HF_PCM_TARGET * 0.15);
    LS_NEAR(-12.0, hf_demod_level_dbfs(&dsp), 1.0);   /* 0.25 = -12 dBFS */
}

LS_CASE(passbands_sit_where_the_mode_says)
{
    int32_t lo, hi;
    hf_demod_passband(HF_DEMOD_USB, 2400, &lo, &hi);
    LS_EQ_INT(250, lo); LS_EQ_INT(2650, hi);
    hf_demod_passband(HF_DEMOD_LSB, 2800, &lo, &hi);
    LS_EQ_INT(-3050, lo); LS_EQ_INT(-250, hi);
    hf_demod_passband(HF_DEMOD_CW, 500, &lo, &hi);
    LS_EQ_INT(-250, lo); LS_EQ_INT(250, hi);
    hf_demod_passband(HF_DEMOD_AM, 8000, &lo, &hi);
    LS_EQ_INT(-4000, lo); LS_EQ_INT(4000, hi);
}
