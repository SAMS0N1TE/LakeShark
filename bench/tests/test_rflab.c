#include "ls_test.h"
#include "rflab_meas.h"   /* app-sdk/apps/rflab: RF LAB is an SD app */

#include <stdint.h>
#include <stdlib.h>

/* RF LAB readings against synthetic I/Q whose answers are known: a tone of
   set power and frequency in Gaussian noise of set density, band-limited
   signals of set width, DC, clipping and a known I/Q gain and phase error. */

#define PI 3.14159265358979323846
#define PAIRS 16384
static rflab_t lab;
static int16_t s16[2 * PAIRS];
static ls_rng_t rng;

static double gauss(void)
{
    double u1 = ((ls_rng_u32(&rng) >> 8) + 1.0) / 16777217.0;
    double u2 = (ls_rng_u32(&rng) >> 8) / 16777216.0;
    return sqrt(-2.0 * log(u1)) * cos(2 * PI * u2);
}

static double dbp(double p) { return 10.0 * log10(p); }

/* amp: tone amplitude (fraction of full scale), noise_db: complex noise
   power dBFS, into s16 at full scale 32768. */
static void make_tone(double fs, double f, double amp, double noise_db)
{
    const double sigma = sqrt(pow(10.0, noise_db / 10.0) / 2.0);
    for (int n = 0; n < PAIRS; ++n) {
        const double ph = 2 * PI * f * n / fs + 0.3;
        double i = amp * cos(ph) + sigma * gauss(), q = amp * sin(ph) + sigma * gauss();
        s16[2 * n] = (int16_t)lrint(i * 32768.0);
        s16[2 * n + 1] = (int16_t)lrint(q * 32768.0);
    }
}

static rflab_cfg_t cfg_at(uint32_t rate)
{
    rflab_cfg_t c = { .rate_hz = rate, .chan_bw_hz = 12500 };
    return c;
}

LS_CASE(tone_power_frequency_noise_snr)
{
    ls_rng_seed(&rng, 11);
    make_tone(125000, 12345.6, 0.5, -60);
    rflab_cfg_t c = cfg_at(125000);
    c.chan_offset_hz = 12345;
    rflab_result_t r;
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_CHECK(r.valid && r.carrier && r.fine);
    LS_EQ_INT(r.frames, PAIRS / RFLAB_N);
    LS_NEAR(r.peak_offset_hz, 12345.6, 2.0);
    LS_NEAR(r.peak_dbfs, dbp(0.25), 0.2);
    LS_NEAR(r.total_dbfs, dbp(0.25 + 1e-6), 0.05);
    LS_NEAR(r.noise_dbfs_hz, -60 - dbp(125000), 0.5);
    LS_NEAR(r.snr_db, dbp(0.25) - (-60 - dbp(125000) + dbp(12500)), 0.6);
    LS_NEAR(r.chan_dbfs, dbp(0.25), 0.2);
    LS_NEAR(r.clip_pct, 0, 1e-9);
    LS_NEAR(r.rbw_hz, 125000.0 / RFLAB_N, 1e-3);
    ls_note("peak %.2f Hz %.2f dBFS, floor %.1f dBFS/Hz, SNR %.1f dB in 12.5k",
            r.peak_offset_hz, r.peak_dbfs, r.noise_dbfs_hz, r.snr_db);
}

/* A carrier that should be at +10 kHz on 100 MHz arrives 250 Hz high: the
   discriminator resolves it to a fraction of the 244 Hz bin. */
LS_CASE(frequency_error_and_ppm)
{
    ls_rng_seed(&rng, 12);
    make_tone(125000, 10250.0, 0.3, -50);
    rflab_cfg_t c = cfg_at(125000);
    c.center_hz = 100000000;
    c.expect_offset_hz = 10000;
    rflab_result_t r;
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_NEAR(r.freq_err_hz, 250.0, 2.0);
    LS_NEAR(r.freq_err_ppm, 250.0 / 100.01e6 * 1e6, 0.02);
    /* At an RTL rate the bin is 4.7 kHz and the reading still holds. */
    make_tone(2400000, -61234.0, 0.3, -50);
    c = cfg_at(2400000);
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_NEAR(r.peak_offset_hz, -61234.0, 20.0);
}

LS_CASE(noise_only_floor_and_occupied_bandwidth)
{
    ls_rng_seed(&rng, 13);
    make_tone(125000, 0, 0.0, -20);
    rflab_cfg_t c = cfg_at(125000);
    rflab_result_t r;
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_CHECK(!r.carrier);
    LS_NEAR(r.total_dbfs, -20, 0.1);
    LS_NEAR(r.noise_dbfs_bin, -20 - dbp(RFLAB_N), 0.4);
    LS_NEAR(r.chan_dbfs, -20 + dbp(12500.0 / 125000.0), 0.4);
    /* White noise fills the span: 99% of it. */
    LS_NEAR(r.obw_hz, 0.99 * 125000, 0.02 * 125000);
}

/* Eighty equal carriers spread over +-15 kHz on a floor 60 dB down: 99% of
   the power sits in 30 kHz plus a bin's spread. */
LS_CASE(occupied_bandwidth_of_a_band)
{
    ls_rng_seed(&rng, 14);
    const double fs = 125000, sigma = sqrt(pow(10.0, -80 / 10.0) / 2.0);
    double ph0[80];
    for (int t = 0; t < 80; ++t) ph0[t] = 2 * PI * (ls_rng_u32(&rng) / 4294967296.0);
    for (int n = 0; n < PAIRS; ++n) {
        double i = sigma * gauss(), q = sigma * gauss();
        for (int t = 0; t < 80; ++t) {
            const double f = -15000 + 30000.0 * (t + 0.5) / 80;
            const double ph = 2 * PI * f * n / fs + ph0[t];
            i += 0.01 * cos(ph); q += 0.01 * sin(ph);
        }
        s16[2 * n] = (int16_t)lrint(i * 32768.0);
        s16[2 * n + 1] = (int16_t)lrint(q * 32768.0);
    }
    rflab_cfg_t c = cfg_at(125000);
    rflab_result_t r;
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_CHECK_MSG(r.obw_hz > 29000 && r.obw_hz < 31500, "obw %.0f", r.obw_hz);
    LS_NEAR(r.obw_lo_hz, -r.obw_hi_hz, 600);
    LS_NEAR(r.total_dbfs, dbp(80 * 1e-4), 0.1);
}

/* DC on both arms, then a carrier driven 40% past full scale: the clip
   count is exactly the pairs with I or Q at a rail. */
LS_CASE(s16_dc_offset_and_clipping)
{
    ls_rng_seed(&rng, 15);
    for (int n = 0; n < PAIRS; ++n) {
        const double ph = 2 * PI * 7000.0 * n / 1024000.0;
        s16[2 * n] = (int16_t)lrint(32768.0 * (0.3 * cos(ph) + 0.1 + 0.01 * gauss()));
        s16[2 * n + 1] = (int16_t)lrint(32768.0 * (0.3 * sin(ph) - 0.05 + 0.01 * gauss()));
    }
    rflab_cfg_t c = cfg_at(1024000);
    rflab_result_t r;
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_NEAR(r.dc_i, 0.1, 0.005);
    LS_NEAR(r.dc_q, -0.05, 0.005);
    LS_NEAR(r.dc_dbfs, dbp(0.01 + 0.0025), 0.4);
    LS_NEAR(r.clip_pct, 0, 1e-9);
    LS_NEAR(r.peak_offset_hz, 7000, 30);

    int rails = 0;
    for (int n = 0; n < PAIRS; ++n) {
        const double ph = 2 * PI * 7000.0 * n / 1024000.0;
        long i = lrint(32768.0 * 1.4 * cos(ph)), q = lrint(32768.0 * 1.4 * sin(ph));
        i = i < -32768 ? -32768 : i > 32767 ? 32767 : i;
        q = q < -32768 ? -32768 : q > 32767 ? 32767 : q;
        s16[2 * n] = (int16_t)i; s16[2 * n + 1] = (int16_t)q;
        rails += (i >= 32767 || i <= -32768 || q >= 32767 || q <= -32768 ||
                  labs(i) >= 32765 || labs(q) >= 32765);
    }
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_CHECK(rails > 0);
    LS_NEAR(r.clip_pct, 100.0 * rails / ((PAIRS / RFLAB_N) * RFLAB_N), 0.01);
}

/* A receiver whose Q arm is 10% hot and 3 degrees off quadrature, seen on a
   carrier (whole cycles in the block, so the statistics are exact) and on
   noise (Q = g (Q0 cos phi + I0 sin phi), within its sampling spread). */
LS_CASE(iq_gain_and_phase_imbalance)
{
    const double g = 1.1, phi = 3.0 * PI / 180;
    for (int n = 0; n < PAIRS; ++n) {
        const double ph = 2 * PI * 64.0 * n / PAIRS;
        s16[2 * n] = (int16_t)lrint(0.4 * cos(ph) * 32768.0);
        s16[2 * n + 1] = (int16_t)lrint(0.4 * g * sin(ph + phi) * 32768.0);
    }
    rflab_cfg_t c = cfg_at(250000);
    rflab_result_t r;
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_NEAR(r.iq_gain_db, -20 * log10(g), 0.02);
    LS_NEAR(r.iq_phase_deg, 3.0, 0.05);
    const double irr = dbp((1 + 2 * g * cos(phi) + g * g) / (1 - 2 * g * cos(phi) + g * g));
    LS_NEAR(r.image_db, irr, 0.2);

    ls_rng_seed(&rng, 16);
    for (int n = 0; n < PAIRS; ++n) {
        const double i0 = 0.1 * gauss(), q0 = 0.1 * gauss();
        s16[2 * n] = (int16_t)lrint(i0 * 32768.0);
        s16[2 * n + 1] = (int16_t)lrint(g * (q0 * cos(phi) + i0 * sin(phi)) * 32768.0);
    }
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    /* One sigma of a correlation over 16384 samples is about 0.45 degrees. */
    LS_NEAR(r.iq_gain_db, -20 * log10(g), 0.15);
    LS_NEAR(r.iq_phase_deg, 3.0, 1.5);
}

/* A strong DC spike at 0 Hz and a weaker carrier: the guard steps the peak
   search over the spike. */
LS_CASE(dc_guard_skips_the_spike)
{
    ls_rng_seed(&rng, 17);
    make_tone(1000000, 50000, 0.05, -70);
    for (int n = 0; n < PAIRS; ++n) { s16[2 * n] += 9000; s16[2 * n + 1] -= 6000; }
    rflab_cfg_t c = cfg_at(1000000);
    rflab_result_t r;
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_NEAR(r.peak_offset_hz, 0, 1000.0 / RFLAB_N * 2);
    c.dc_guard_bins = 2;
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_NEAR(r.peak_offset_hz, 50000, 20);
    LS_NEAR(r.peak_dbfs, dbp(0.0025), 0.3);
}

LS_CASE(too_short_and_spectrum_view)
{
    rflab_cfg_t c = cfg_at(125000);
    rflab_result_t r;
    LS_CHECK(!rflab_measure(&lab, &c, s16, RFLAB_N - 1, &r));
    LS_CHECK(!r.valid);
    ls_rng_seed(&rng, 18);
    make_tone(125000, 31250, 0.5, -60);   /* a quarter of the way up */
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    float cols[64];
    LS_CHECK(rflab_spectrum_db(&lab, cols, 64));
    int best = 0;
    for (int i = 1; i < 64; ++i) if (cols[i] > cols[best]) best = i;
    LS_EQ_INT(best, 48);                  /* 0 Hz at 32, +fs/4 at 48 */
}

/* A carrier 40 dB over a white floor: the 99% bandwidth is the carrier's
   (its window's main lobe), not 99% of the span the floor fills; on noise
   alone there is nothing above the floor and the raw span is reported. */
LS_CASE(occupied_bandwidth_is_of_the_signal_not_the_floor)
{
    ls_rng_seed(&rng, 19);
    make_tone(125000, -20000, 0.3, -30);
    rflab_cfg_t c = cfg_at(125000);
    rflab_result_t r;
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_CHECK(r.obw_net);
    LS_CHECK_MSG(r.obw_hz < 6 * r.rbw_hz, "obw %.0f Hz", r.obw_hz);
    LS_NEAR((r.obw_lo_hz + r.obw_hi_hz) / 2, -20000, 2 * r.rbw_hz);
    make_tone(125000, 0, 0.0, -30);
    LS_CHECK(rflab_measure(&lab, &c, s16, PAIRS, &r));
    LS_CHECK(!r.obw_net);
}
