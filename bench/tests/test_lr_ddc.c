#include "ls_test.h"
#include "lr_ddc.h"
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* The DDC between the LR2021's wide I/Q and the 125k consumers. Tones are
   synthesized at each driver rate and measured at the output: the channel
   lands at 0 Hz at its input level, the passband is flat, out-of-channel
   tones and the LO's DC leakage do not alias in, and the output count is
   exact over long runs whatever the input block sizes. */

#define OUT 125000.0
#define AMP 8000.0
static const uint32_t rates[] = {500000, 576000, 666667, 800000, 1000000};
#define NRATES (sizeof(rates) / sizeof(rates[0]))

static lr_ddc_t ddc, ddc2;
static int16_t inbuf[2 * 4096];
static int16_t outbuf[2 * 8192];

static double fin_of(uint32_t hz) { return hz == 666667 ? 2e6 / 3 : hz; }

/* Settle, then measure. Returns the output pairs of the measured span in
   outbuf. Input is one complex tone at f_hz (plus optional DC), fed in
   driver-sized blocks of 384 pairs. */
static size_t run_tone(uint32_t rate, int32_t offset, double f_hz, double amp,
                       double dc_i, double dc_q, double settle_s, double meas_s)
{
    LS_CHECK(lr_ddc_init(&ddc, rate, offset, LR_DDC_OUT_HZ));
    const double fs = fin_of(rate), w = 2 * 3.14159265358979323846 * f_hz / fs;
    const size_t settle = (size_t)(settle_s * fs), total = settle + (size_t)(meas_s * fs);
    size_t n = 0, got = 0, skip = (size_t)(settle_s * OUT);
    while (n < total) {
        size_t blk = total - n < 384 ? total - n : 384;
        for (size_t k = 0; k < blk; ++k) {
            double ph = w * (double)(n + k);
            inbuf[2 * k] = (int16_t)lrint(amp * cos(ph) + dc_i);
            inbuf[2 * k + 1] = (int16_t)lrint(amp * sin(ph) + dc_q);
        }
        int16_t tmp[2 * 128];
        size_t m = lr_ddc_process(&ddc, inbuf, blk, tmp, 128);
        for (size_t k = 0; k < m; ++k) {
            if (skip) { --skip; continue; }
            if (got < sizeof(outbuf) / 4) {
                outbuf[2 * got] = tmp[2 * k]; outbuf[2 * got + 1] = tmp[2 * k + 1]; ++got;
            }
        }
        n += blk;
    }
    LS_EQ_UINT(0, ddc.dropped);
    return got;
}

/* Complex amplitude at f_hz in the measured output. */
static double tone_amp(size_t n, double f_hz)
{
    double re = 0, im = 0, w = 2 * 3.14159265358979323846 * f_hz / OUT;
    for (size_t k = 0; k < n; ++k) {
        double c = cos(w * k), s = -sin(w * k), x = outbuf[2 * k], y = outbuf[2 * k + 1];
        re += x * c - y * s; im += x * s + y * c;
    }
    return hypot(re, im) / (double)n;
}

static double power(size_t n)
{
    double p = 0;
    for (size_t k = 0; k < n; ++k)
        p += (double)outbuf[2 * k] * outbuf[2 * k] + (double)outbuf[2 * k + 1] * outbuf[2 * k + 1];
    return p / (double)n;
}

static double db(double x) { return 20 * log10(x); }

LS_CASE(init_rejects_bad_configs)
{
    LS_CHECK(!lr_ddc_init(&ddc, 200000, 0, LR_DDC_OUT_HZ));     /* < 2x out */
    LS_CHECK(!lr_ddc_init(&ddc, 2000000, 0, LR_DDC_OUT_HZ));    /* >= 16x out */
    LS_CHECK(!lr_ddc_init(&ddc, 500000, 200000, LR_DDC_OUT_HZ)); /* past Nyquist */
    LS_CHECK(lr_ddc_init(&ddc, 500000, -187500, LR_DDC_OUT_HZ));
    LS_CHECK(lr_ddc_init(&ddc, 1000000, 250000, LR_DDC_OUT_HZ));
    LS_CHECK(!lr_ddc_set_offset(&ddc, 450000));
    LS_EQ_UINT(LR_DDC_OUT_HZ, lr_ddc_out_hz(&ddc));
    for (unsigned r = 0; r < NRATES; ++r) {
        LS_CHECK(lr_ddc_init(&ddc, rates[r], 100000, LR_DDC_OUT_HZ));
        ls_note("%7u: CIC%u /%u, channel %u taps x %u rows, step %u+%u/%u, state %u B",
                (unsigned)rates[r], LR_DDC_CIC_N, (unsigned)ddc.decim, ddc.nt,
                LR_DDC_PHASES + 1, (unsigned)ddc.step_int, (unsigned)ddc.step_num,
                (unsigned)ddc.step_den, (unsigned)sizeof(ddc));
    }
}

/* (a) The tone at the channel offset comes out as a steady phasor at 0 Hz,
   at the input amplitude, for both offset signs. */
LS_CASE(channel_tone_lands_at_dc_at_input_level)
{
    const int32_t offs[] = {150000, -120000, 0};
    for (unsigned r = 0; r < NRATES; ++r)
        for (unsigned o = 0; o < 3; ++o) {
            /* At offset 0 the DC blocker would eat a carrier at exactly 0 Hz;
               put it 1 kHz up, and let the blocker's start-up step (time
               constant 4..8 ms) die away before measuring. */
            double f = offs[o] ? 0 : 1000;
            size_t n = run_tone(rates[r], offs[o], offs[o] + f, AMP, 0, 0,
                                offs[o] ? 0.01 : 0.1, 0.04);
            double a = tone_amp(n, f), p = power(n);
            double resid = p - a * a;
            if (resid < 1e-9) resid = 1e-9;
            ls_note("%7u off %+7d: level %+.3f dB, residue %.1f dBc",
                    (unsigned)rates[r], (int)offs[o], db(a / AMP), 10 * log10(resid / (a * a)));
            LS_NEAR(db(a / AMP), 0, 0.1);
            LS_CHECK(10 * log10(resid / (a * a)) < -60);
        }
}

/* An in-channel tone off centre stays one clean tone at its own frequency:
   the fractional rates' row blending adds no audible images. */
LS_CASE(in_channel_tone_stays_clean)
{
    const double fs[] = {8000, -12500, 19000};
    for (unsigned r = 0; r < NRATES; ++r)
        for (unsigned k = 0; k < 3; ++k) {
            size_t n = run_tone(rates[r], 150000, 150000 + fs[k], AMP, 0, 0, 0.01, 0.06);
            double a = tone_amp(n, fs[k]), p = power(n), resid = p - a * a;
            if (resid < 1e-9) resid = 1e-9;
            ls_note("%7u tone %+6.0f Hz: level %+.3f dB, everything else %.1f dBc",
                    (unsigned)rates[r], fs[k], db(a / AMP), 10 * log10(resid / (a * a)));
            LS_CHECK(10 * log10(resid / (a * a)) < -55);
        }
}

/* (b) Flat over the channel: +-8 kHz within 1 dB (and the wider +-20 kHz
   design passband is reported). */
LS_CASE(passband_ripple)
{
    for (unsigned r = 0; r < NRATES; ++r) {
        double lo8 = 1e9, hi8 = -1e9, lo20 = 1e9, hi20 = -1e9;
        for (int f = -20000; f <= 20000; f += 1000) {
            size_t n = run_tone(rates[r], 150000, 150000 + f, AMP, 0, 0, 0.004, 0.016);
            double g = db(tone_amp(n, f) / AMP);
            if (abs(f) <= 8000) { if (g < lo8) lo8 = g; if (g > hi8) hi8 = g; }
            if (g < lo20) lo20 = g;
            if (g > hi20) hi20 = g;
        }
        ls_note("%7u: ripple +-8k %.3f dB, +-20k %.3f dB", (unsigned)rates[r],
                hi8 - lo8, hi20 - lo20);
        LS_CHECK(hi8 - lo8 <= 1.0);
        LS_CHECK(hi8 - lo8 <= 0.1); /* the design is far flatter than required */
    }
}

/* (c) A tone 50 or 150 kHz past the channel edge, at any frequency the input
   can carry, leaves at least 50 dB less total power in the output: nothing
   aliases in. Swept 62.5 kHz..Nyquist on both sides at 2.5 kHz steps. */
LS_CASE(out_of_channel_tones_rejected)
{
    for (unsigned r = 0; r < NRATES; ++r) {
        const double fs = fin_of(rates[r]);
        const int32_t off = rates[r] == 500000 ? 100000 : 150000;
        double worst = -1e9, at = 0, w50 = -1e9, w150 = -1e9;
        for (int side = -1; side <= 1; side += 2)
            for (double d = 62500; d < fs; d += 2500) {
                double f = off + side * d;
                if (fabs(f) > fs / 2 - 1000) continue;
                size_t n = run_tone(rates[r], off, f, AMP, 0, 0, 0.003, 0.008);
                double rej = 10 * log10(power(n) / (AMP * AMP) + 1e-12);
                if (rej > worst) { worst = rej; at = side * d; }
                if (d == 62500 && rej > w50) w50 = rej;
                if (d == 162500 && rej > w150) w150 = rej;
            }
        ls_note("%7u off %d: +-50k past edge %.1f dB, +-150k %.1f dB, worst %.1f dB at %+.1f kHz",
                (unsigned)rates[r], (int)off, w50, w150, worst, at / 1000);
        LS_CHECK(w50 <= -50);
        LS_CHECK(w150 <= -50);
        LS_CHECK(worst <= -50);
    }
}

/* (c) Input DC, the LO's leakage, does not reach the output: with the LO off
   channel it is mixed out of band and filtered, and the DC blocker removes
   it outright even at offset 0. */
LS_CASE(input_dc_rejected)
{
    const double pdc = 3000.0 * 3000 + 2000.0 * 2000;
    for (unsigned r = 0; r < NRATES; ++r) {
        /* Filters only (blocker off past out). */
        size_t n = run_tone(rates[r], 150000, 0, 0, 3000, -2000, 0.05, 0.02);
        LS_CHECK(!ddc.dc_on);
        double far = 10 * log10(power(n) / pdc + 1e-12);
        /* Blocker on: DC lands 70 kHz off, then at the channel itself. */
        n = run_tone(rates[r], 70000, 0, 0, 3000, -2000, 0.05, 0.02);
        LS_CHECK(ddc.dc_on);
        double near = 10 * log10(power(n) / pdc + 1e-12);
        n = run_tone(rates[r], 0, 0, 0, 3000, -2000, 0.1, 0.02);
        double on = 10 * log10(power(n) / pdc + 1e-12);
        /* A channel tone riding on the DC keeps its level. */
        n = run_tone(rates[r], 150000, 151000, AMP, 3000, -2000, 0.05, 0.02);
        double lvl = db(tone_amp(n, 1000) / AMP);
        ls_note("%7u: DC at offset 150k %.1f dB, 70k %.1f dB, 0 %.1f dB; tone %+.3f dB",
                (unsigned)rates[r], far, near, on, lvl);
        LS_CHECK(far <= -50);
        LS_CHECK(near <= -50);
        LS_CHECK(on <= -50);
        LS_NEAR(lvl, 0, 0.1);
    }
}

/* (d) Output count tracks in * 125k / Fin to within 2 pairs at every block
   over ten seconds: the fractional rates do not drift. */
LS_CASE(output_count_exact_over_long_runs)
{
    static int16_t big[2 * 1000];
    ls_rng_t rng; ls_rng_seed(&rng, 7);
    for (size_t k = 0; k < 2000; ++k) big[k] = (int16_t)(ls_rng_u32(&rng) & 0x3fff) - 0x2000;
    for (unsigned r = 0; r < NRATES; ++r) {
        LS_CHECK(lr_ddc_init(&ddc, rates[r], 150000, LR_DDC_OUT_HZ));
        const double fs = fin_of(rates[r]);
        uint64_t in = 0, out = 0; double worst = 0;
        const uint64_t total = (uint64_t)(10 * fs);
        while (in < total) {
            size_t blk = 1000, cap = lr_ddc_out_max(&ddc, blk);
            out += lr_ddc_process(&ddc, big, blk, outbuf, cap);
            in += blk;
            double err = (double)out - (double)in * OUT / fs;
            if (fabs(err) > fabs(worst)) worst = err;
        }
        ls_note("%7u: %llu in -> %llu out over 10 s, worst error %+.2f pairs",
                (unsigned)rates[r], (unsigned long long)in, (unsigned long long)out, worst);
        LS_CHECK(fabs(worst) <= 2);
        LS_EQ_UINT(0, ddc.dropped);
    }
}

/* (d) Any block cut gives bit-identical output to one big block. */
LS_CASE(block_size_independent)
{
    enum { N = 60000 };
    static int16_t sig[2 * N], a[2 * N], b[2 * N];
    ls_rng_t rng; ls_rng_seed(&rng, 99);
    for (size_t k = 0; k < N; ++k) {
        double t = (double)k;
        sig[2 * k] = (int16_t)lrint(4000 * cos(0.9 * t) + 2000 * cos(0.031 * t) + 500 +
                                    300 * ls_rng_noise(&rng));
        sig[2 * k + 1] = (int16_t)lrint(4000 * sin(0.9 * t) - 2000 * sin(0.17 * t) - 300 +
                                        300 * ls_rng_noise(&rng));
    }
    for (unsigned r = 0; r < NRATES; ++r) {
        LS_CHECK(lr_ddc_init(&ddc, rates[r], 140000, LR_DDC_OUT_HZ));
        LS_CHECK(lr_ddc_init(&ddc2, rates[r], 140000, LR_DDC_OUT_HZ));
        size_t na = lr_ddc_process(&ddc, sig, N, a, N), nb = 0;
        for (size_t k = 0; k < N;) {
            size_t blk = 1 + ls_rng_u32(&rng) % 997;
            if (blk > N - k) blk = N - k;
            nb += lr_ddc_process(&ddc2, sig + 2 * k, blk, b + 2 * nb, N - nb);
            k += blk;
        }
        LS_EQ_UINT(na, nb);
        LS_CHECK(na > 0 && memcmp(a, b, na * 4) == 0);
    }
}

/* Retune keeps the stream; reset clears it to the just-initialised state. */
LS_CASE(retune_and_reset)
{
    size_t n = run_tone(800000, 150000, -100000, AMP, 0, 0, 0.0, 0.0);
    (void)n;
    LS_CHECK(lr_ddc_set_offset(&ddc, -100000));
    LS_EQ_INT(-100000, ddc.offset_hz);
    lr_ddc_reset(&ddc);
    LS_CHECK(lr_ddc_init(&ddc2, 800000, -100000, LR_DDC_OUT_HZ));
    for (size_t k = 0; k < 2000; ++k) inbuf[k] = (int16_t)(k * 37 % 4001 - 2000);
    size_t na = lr_ddc_process(&ddc, inbuf, 1000, outbuf, 4096);
    static int16_t other[2 * 4096];
    size_t nb = lr_ddc_process(&ddc2, inbuf, 1000, other, 4096);
    LS_EQ_UINT(na, nb);
    LS_CHECK(memcmp(outbuf, other, na * 4) == 0);
}

/* ---- The in/4 fast path: offset exactly +-in/4 mixes by sign and swap. ---- */
static const uint32_t qrates[] = {500000, 576000, 1000000};
#define NQRATES (sizeof(qrates) / sizeof(qrates[0]))

/* +-in/4 engages it, from init and after reset; anything else keeps the
   oscillator. Every listed rate gets one exact row per step phase. */
LS_CASE(quarter_path_engages_only_at_in_over_4)
{
    for (unsigned r = 0; r < NQRATES; ++r) {
        const int32_t q = (int32_t)(qrates[r] / 4);
        LS_CHECK(lr_ddc_init(&ddc, qrates[r], q, LR_DDC_OUT_HZ));
        LS_EQ_INT(1, ddc.quarter);
        LS_CHECK(!ddc.dc_on);
        LS_CHECK(lr_ddc_init(&ddc, qrates[r], -q, LR_DDC_OUT_HZ));
        LS_EQ_INT(-1, ddc.quarter);
        lr_ddc_reset(&ddc);
        LS_EQ_INT(-1, ddc.quarter);
        LS_CHECK(lr_ddc_init(&ddc, qrates[r], q - 1000, LR_DDC_OUT_HZ));
        LS_EQ_INT(0, ddc.quarter);
    }
    LS_CHECK(lr_ddc_init(&ddc, 800000, 200000, LR_DDC_OUT_HZ));
    LS_EQ_INT(1, ddc.quarter); /* R = 3 takes the integer CIC with the same mix */
    for (unsigned r = 0; r < NRATES; ++r) {
        LS_CHECK(lr_ddc_init(&ddc, rates[r], 100000, LR_DDC_OUT_HZ));
        LS_CHECK(ddc.exact);
        LS_EQ_UINT(ddc.step_den, ddc.rows);
    }
}

/* (a) The channel tone lands at 0 Hz at its input level, both signs; an
   in-channel tone off centre stays one clean tone. */
LS_CASE(quarter_tone_lands_at_dc_and_stays_clean)
{
    const double fs[] = {0, 8000, -12500, 19000};
    for (unsigned r = 0; r < NQRATES; ++r)
        for (int sign = -1; sign <= 1; sign += 2) {
            const int32_t off = sign * (int32_t)(qrates[r] / 4);
            for (unsigned k = 0; k < 4; ++k) {
                size_t n = run_tone(qrates[r], off, off + fs[k], AMP, 0, 0, 0.01, 0.04);
                LS_EQ_INT(sign, ddc.quarter);
                double a = tone_amp(n, fs[k]), p = power(n), resid = p - a * a;
                if (resid < 1e-9) resid = 1e-9;
                ls_note("%7u off %+7d tone %+6.0f Hz: level %+.3f dB, everything else %.1f dBc",
                        (unsigned)qrates[r], (int)off, fs[k], db(a / AMP),
                        10 * log10(resid / (a * a)));
                LS_NEAR(db(a / AMP), 0, 0.1);
                LS_CHECK(10 * log10(resid / (a * a)) < (fs[k] ? -55 : -60));
            }
        }
}

/* (b) Flat over the channel. */
LS_CASE(quarter_passband_ripple)
{
    for (unsigned r = 0; r < NQRATES; ++r) {
        const int32_t off = (int32_t)(qrates[r] / 4);
        double lo8 = 1e9, hi8 = -1e9;
        for (int f = -8000; f <= 8000; f += 1000) {
            size_t n = run_tone(qrates[r], off, off + f, AMP, 0, 0, 0.004, 0.016);
            double g = db(tone_amp(n, f) / AMP);
            if (g < lo8) lo8 = g;
            if (g > hi8) hi8 = g;
        }
        ls_note("%7u off %d: ripple +-8k %.3f dB", (unsigned)qrates[r], (int)off, hi8 - lo8);
        LS_CHECK(hi8 - lo8 <= 0.1);
    }
}

/* (c) Out-of-channel tones anywhere the input can carry them, from the
   channel edge out, and the LO's DC (mixed to -in/4, past the channel)
   stay out of the output. */
LS_CASE(quarter_rejects_out_of_channel_tones_and_dc)
{
    for (unsigned r = 0; r < NQRATES; ++r) {
        const double fs = fin_of(qrates[r]);
        const int32_t off = (int32_t)(qrates[r] / 4);
        double worst = -1e9, at = 0;
        for (int side = -1; side <= 1; side += 2)
            for (double d = 62500; d < fs; d += 2500) {
                double f = off + side * d;
                if (fabs(f) > fs / 2 - 1000) continue;
                size_t n = run_tone(qrates[r], off, f, AMP, 0, 0, 0.003, 0.008);
                double rej = 10 * log10(power(n) / (AMP * AMP) + 1e-12);
                if (rej > worst) { worst = rej; at = side * d; }
            }
        const double pdc = 3000.0 * 3000 + 2000.0 * 2000;
        size_t n = run_tone(qrates[r], off, 0, 0, 3000, -2000, 0.05, 0.02);
        double dc = 10 * log10(power(n) / pdc + 1e-12);
        n = run_tone(qrates[r], off, off + 1000, AMP, 3000, -2000, 0.05, 0.02);
        double lvl = db(tone_amp(n, 1000) / AMP);
        ls_note("%7u off %d: worst tone %.1f dB at %+.1f kHz, DC %.1f dB, tone on DC %+.3f dB",
                (unsigned)qrates[r], (int)off, worst, at / 1000, dc, lvl);
        LS_CHECK(worst <= -60);
        LS_CHECK(dc <= -50);
        LS_NEAR(lvl, 0, 0.1);
    }
}

/* (d) Exact output count, and any block cut gives the same bits. */
LS_CASE(quarter_counts_exact_and_block_size_independent)
{
    enum { N = 60000 };
    static int16_t sig[2 * N], a[2 * N], b[2 * N];
    ls_rng_t rng; ls_rng_seed(&rng, 123);
    for (size_t k = 0; k < N; ++k) {
        double t = (double)k;
        sig[2 * k] = (int16_t)lrint(4000 * cos(0.9 * t) + 2000 * cos(1.6 * t) + 500 +
                                    300 * ls_rng_noise(&rng));
        sig[2 * k + 1] = (int16_t)lrint(4000 * sin(0.9 * t) - 2000 * sin(0.17 * t) - 300 +
                                        300 * ls_rng_noise(&rng));
    }
    for (unsigned r = 0; r < NQRATES; ++r)
        for (int sign = -1; sign <= 1; sign += 2) {
            const int32_t off = sign * (int32_t)(qrates[r] / 4);
            LS_CHECK(lr_ddc_init(&ddc, qrates[r], off, LR_DDC_OUT_HZ));
            LS_CHECK(lr_ddc_init(&ddc2, qrates[r], off, LR_DDC_OUT_HZ));
            size_t na = lr_ddc_process(&ddc, sig, N, a, N), nb = 0;
            for (size_t k = 0; k < N;) {
                size_t blk = 1 + ls_rng_u32(&rng) % 997;
                if (blk > N - k) blk = N - k;
                nb += lr_ddc_process(&ddc2, sig + 2 * k, blk, b + 2 * nb, N - nb);
                k += blk;
            }
            LS_EQ_UINT(na, nb);
            LS_CHECK(na > 0 && memcmp(a, b, na * 4) == 0);
            /* N in -> N * 125k / in out, within the filter's start. */
            const double want = (double)N * OUT / fin_of(qrates[r]);
            LS_CHECK(fabs((double)na - want) <= 2);
        }
    static int16_t big[2 * 1000];
    for (size_t k = 0; k < 2000; ++k) big[k] = (int16_t)(ls_rng_u32(&rng) & 0x3fff) - 0x2000;
    for (unsigned r = 0; r < NQRATES; ++r) {
        LS_CHECK(lr_ddc_init(&ddc, qrates[r], (int32_t)(qrates[r] / 4), LR_DDC_OUT_HZ));
        const double fs = fin_of(qrates[r]);
        uint64_t in = 0, out = 0; double worst = 0;
        while (in < (uint64_t)(10 * fs)) {
            size_t blk = 1000, cap = lr_ddc_out_max(&ddc, blk);
            out += lr_ddc_process(&ddc, big, blk, outbuf, cap);
            in += blk;
            double err = (double)out - (double)in * OUT / fs;
            if (fabs(err) > fabs(worst)) worst = err;
        }
        LS_CHECK(fabs(worst) <= 2);
        LS_EQ_UINT(0, ddc.dropped);
    }
}

/* The sign-and-swap mix is the oscillator's own mix: forcing the same
   offset through the oscillator gives the same output to within its
   rounding (R = 2 in float; R > 2 truncates each mixed sample). */
LS_CASE(quarter_path_matches_the_oscillator)
{
    enum { N = 30000 };
    static int16_t sig[2 * N], a[2 * N], b[2 * N];
    static const uint32_t all[] = {500000, 576000, 800000, 1000000};
    ls_rng_t rng; ls_rng_seed(&rng, 5);
    for (size_t k = 0; k < 2 * N; ++k) sig[k] = (int16_t)(ls_rng_u32(&rng) & 0x7fff) - 0x4000;
    for (unsigned r = 0; r < 4; ++r)
        for (int sign = -1; sign <= 1; sign += 2) {
            const int32_t off = sign * (int32_t)(all[r] / 4);
            LS_CHECK(lr_ddc_init(&ddc, all[r], off, LR_DDC_OUT_HZ));
            LS_CHECK(lr_ddc_init(&ddc2, all[r], off, LR_DDC_OUT_HZ));
            LS_EQ_INT(sign, ddc.quarter);
            ddc2.quarter = 0; /* same offset, oscillator path */
            size_t na = lr_ddc_process(&ddc, sig, N, a, N);
            size_t nb = lr_ddc_process(&ddc2, sig, N, b, N);
            LS_EQ_UINT(na, nb);
            int worst = 0;
            for (size_t k = 0; k < 2 * na; ++k) {
                int d = abs(a[k] - b[k]);
                if (d > worst) worst = d;
            }
            ls_note("%7u off %+7d: %u outputs, largest difference %d LSB",
                    (unsigned)all[r], (int)off, (unsigned)na, worst);
            LS_CHECK(worst <= (ddc.decim == 2 ? 1 : 4));
        }
}

/* Retuning away from in/4 and back mid-stream never jumps the phase: the
   path stays on the oscillator unless the phase lines up, and the channel
   still lands at 0 Hz at level. */
LS_CASE(quarter_retune_keeps_the_stream)
{
    const double w = 2 * 3.14159265358979323846 * 144000.0 / 576000;
    LS_CHECK(lr_ddc_init(&ddc, 576000, 144000, LR_DDC_OUT_HZ));
    LS_EQ_INT(1, ddc.quarter);
    size_t n = 0, got = 0;
    for (int phase = 0; phase < 3; ++phase) {
        if (phase == 1) { LS_CHECK(lr_ddc_set_offset(&ddc, 100000)); LS_EQ_INT(0, ddc.quarter); }
        if (phase == 2) LS_CHECK(lr_ddc_set_offset(&ddc, 144000));
        for (int blk = 0; blk < 30; ++blk) {
            for (size_t k = 0; k < 384; ++k) {
                double ph = w * (double)(n + k);
                inbuf[2 * k] = (int16_t)lrint(AMP * cos(ph));
                inbuf[2 * k + 1] = (int16_t)lrint(AMP * sin(ph));
            }
            n += 384;
            int16_t tmp[2 * 128];
            size_t m = lr_ddc_process(&ddc, inbuf, 384, tmp, 128);
            if (phase == 2 && blk >= 10)
                for (size_t k = 0; k < m && got < sizeof(outbuf) / 4; ++k, ++got) {
                    outbuf[2 * got] = tmp[2 * k]; outbuf[2 * got + 1] = tmp[2 * k + 1];
                }
        }
    }
    /* back on 144k: the tone is at 0 Hz again, at its level */
    LS_NEAR(db(tone_amp(got, 0) / AMP), 0, 0.1);
    LS_EQ_UINT(0, ddc.dropped);
}

/* Host cost per input sample, for the record. The P4 estimate is in the
   module notes; this only guards against an accidental slowdown. */
LS_CASE(host_timing)
{
    static int16_t sig[2 * 4096];
    ls_rng_t rng; ls_rng_seed(&rng, 3);
    for (size_t k = 0; k < 2 * 4096; ++k) sig[k] = (int16_t)(ls_rng_u32(&rng) & 0x3fff) - 0x2000;
    for (unsigned r = 0; r < NRATES; ++r) {
        LS_CHECK(lr_ddc_init(&ddc, rates[r], 150000, LR_DDC_OUT_HZ));
        const unsigned reps = 500;
        clock_t t0 = clock();
        for (unsigned k = 0; k < reps; ++k) lr_ddc_process(&ddc, sig, 4096, outbuf, 8192);
        double ns = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e9 / (reps * 4096.0);
        ls_note("%7u: host %.1f ns per input pair (%.2f%% of a host core at rate)",
                (unsigned)rates[r], ns, ns * fin_of(rates[r]) / 1e7);
    }
}
