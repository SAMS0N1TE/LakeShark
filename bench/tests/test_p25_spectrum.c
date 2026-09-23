/* LS_TEST_SOURCES: ${APP}/p25/p25_spectrum.c
 * ${FW}/components/lakeshark/dsp/spectrum.c */

#include "ls_test.h"

#include "p25_controls.h"
#include "p25_spectrum.h"

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint8_t s_iq[P25_SPECTRUM_BINS * 2];

static void make_tone(int offset_bin)
{
    for (int i = 0; i < P25_SPECTRUM_BINS; i++) {
        double phase = 2.0 * M_PI * (double)offset_bin * (double)i /
                       (double)P25_SPECTRUM_BINS;
        s_iq[i * 2]     = (uint8_t)(127.5 + 80.0 * cos(phase));
        s_iq[i * 2 + 1] = (uint8_t)(127.5 + 80.0 * sin(phase));
    }
}

static p25_spectrum_feed_result_t feed_n(int count, bool following,
                                          uint32_t now_ms)
{
    p25_spectrum_feed_result_t result = P25_SPECTRUM_FEED_INVALID;
    for (int i = 0; i < count; i++) {
        result = p25_spectrum_feed_iq(
            P25_SPECTRUM_OWNER_RX, s_iq, sizeof(s_iq),
            851012500u, 240000u, 200000u, following, now_ms);
    }
    return result;
}

LS_CASE(pixel_frequency_mapping_includes_endpoints_and_center)
{
    uint32_t hz = 0;
    LS_CHECK(p25_spectrum_tap_hz(0, 241, 851000000u, 240000u,
                                 P25_CONTROL_TUNER_MIN_HZ,
                                 P25_CONTROL_TUNER_MAX_HZ, &hz));
    LS_EQ_UINT(hz, 850880000u);
    LS_CHECK(p25_spectrum_tap_hz(120, 241, 851000000u, 240000u,
                                 P25_CONTROL_TUNER_MIN_HZ,
                                 P25_CONTROL_TUNER_MAX_HZ, &hz));
    LS_EQ_UINT(hz, 851000000u);
    LS_CHECK(p25_spectrum_tap_hz(240, 241, 851000000u, 240000u,
                                 P25_CONTROL_TUNER_MIN_HZ,
                                 P25_CONTROL_TUNER_MAX_HZ, &hz));
    LS_EQ_UINT(hz, 851120000u);
}

LS_CASE(pixel_and_tuner_bounds_reject_instead_of_clamping)
{
    uint32_t hz = 123u;
    LS_CHECK(!p25_spectrum_tap_hz(-1, 240, 851000000u, 240000u,
                                  P25_CONTROL_TUNER_MIN_HZ,
                                  P25_CONTROL_TUNER_MAX_HZ, &hz));
    LS_CHECK(!p25_spectrum_tap_hz(240, 240, 851000000u, 240000u,
                                  P25_CONTROL_TUNER_MIN_HZ,
                                  P25_CONTROL_TUNER_MAX_HZ, &hz));
    LS_CHECK(!p25_spectrum_tap_hz(0, 1, 851000000u, 240000u,
                                  P25_CONTROL_TUNER_MIN_HZ,
                                  P25_CONTROL_TUNER_MAX_HZ, &hz));
    LS_CHECK(!p25_spectrum_tap_hz(0, 101, P25_CONTROL_TUNER_MIN_HZ,
                                  240000u, P25_CONTROL_TUNER_MIN_HZ,
                                  P25_CONTROL_TUNER_MAX_HZ, &hz));
    LS_CHECK(!p25_spectrum_tap_hz(100, 101, P25_CONTROL_TUNER_MAX_HZ,
                                  240000u, P25_CONTROL_TUNER_MIN_HZ,
                                  P25_CONTROL_TUNER_MAX_HZ, &hz));
    LS_EQ_UINT(hz, 123u);
}

LS_CASE(only_the_p25_rx_owner_can_publish_samples)
{
    make_tone(64);
    p25_spectrum_init();
    p25_spectrum_enable(true);

    LS_EQ_INT(p25_spectrum_feed_iq(
                  P25_SPECTRUM_OWNER_NONE, s_iq, sizeof(s_iq),
                  851012500u, 240000u, 200000u, false, 10u),
              P25_SPECTRUM_FEED_NOT_OWNER);
    float bins[16];
    LS_CHECK(!p25_spectrum_read(bins, 16, 10u,
                                P25_SPECTRUM_STALE_MS, NULL));

    p25_spectrum_enable(false);
    LS_EQ_INT(p25_spectrum_feed_iq(
                  P25_SPECTRUM_OWNER_RX, s_iq, sizeof(s_iq),
                  851012500u, 240000u, 200000u, false, 10u),
              P25_SPECTRUM_FEED_DISABLED);
}

LS_CASE(sample_work_is_bounded_and_traffic_iq_is_publishable)
{
    make_tone(64); /* +30 kHz at 240 kSPS */
    p25_spectrum_init();
    p25_spectrum_enable(true);

    float bins[128];
    for (int i = 1; i < P25_SPECTRUM_BLOCK_STRIDE; i++) {
        LS_EQ_INT(feed_n(1, true, 100u), P25_SPECTRUM_FEED_DEFERRED);
        LS_CHECK(!p25_spectrum_read(bins, 128, 100u,
                                    P25_SPECTRUM_STALE_MS, NULL));
    }
    LS_EQ_INT(feed_n(1, true, 100u), P25_SPECTRUM_FEED_PUBLISHED);

    p25_spectrum_snapshot_t snapshot;
    LS_CHECK(p25_spectrum_read(bins, 128, 100u,
                               P25_SPECTRUM_STALE_MS, &snapshot));
    LS_EQ_UINT(snapshot.center_hz, 851012500u);
    LS_EQ_UINT(snapshot.span_hz, 240000u);
    LS_EQ_UINT(snapshot.filter_hz, 200000u);
    LS_EQ_UINT(snapshot.blocks_per_fft, P25_SPECTRUM_BLOCK_STRIDE);
    LS_EQ_UINT(snapshot.fft_bins, P25_SPECTRUM_BINS);
    LS_CHECK(snapshot.following_voice);

    int peak = 0;
    for (int i = 1; i < 128; i++)
        if (bins[i] > bins[peak]) peak = i;
    /* Shifted bin 320 is output group 80. A one-bin tolerance admits the
     * Hann main lobe while still proving this is real IQ frequency content. */
    LS_CHECK(peak >= 79 && peak <= 81);
    LS_CHECK(bins[peak] > 0.50f);
}

LS_CASE(stale_disable_reenable_and_retune_invalidate_publication)
{
    make_tone(-32);
    p25_spectrum_init();
    p25_spectrum_enable(true);
    LS_EQ_INT(feed_n(P25_SPECTRUM_BLOCK_STRIDE, false, 100u),
              P25_SPECTRUM_FEED_PUBLISHED);

    float bins[32];
    p25_spectrum_snapshot_t snapshot;
    LS_CHECK(p25_spectrum_read(bins, 32, 100u + P25_SPECTRUM_STALE_MS,
                               P25_SPECTRUM_STALE_MS, &snapshot));
    LS_CHECK(!p25_spectrum_read(bins, 32,
                                101u + P25_SPECTRUM_STALE_MS,
                                P25_SPECTRUM_STALE_MS, &snapshot));

    p25_spectrum_enable(false);
    LS_CHECK(!p25_spectrum_read(bins, 32, 100u,
                                P25_SPECTRUM_STALE_MS, &snapshot));
    p25_spectrum_enable(true);
    LS_CHECK(!p25_spectrum_read(bins, 32, 100u,
                                P25_SPECTRUM_STALE_MS, &snapshot));

    LS_EQ_INT(feed_n(P25_SPECTRUM_BLOCK_STRIDE, false, 200u),
              P25_SPECTRUM_FEED_PUBLISHED);
    p25_spectrum_invalidate();
    LS_CHECK(!p25_spectrum_read(bins, 32, 200u,
                                P25_SPECTRUM_STALE_MS, &snapshot));
}

LS_CASE(the_lo_spur_at_centre_is_notched_out)
{
    /* A tone at bin 0 is exactly what the tuner's LO leakage looks like: all
       the energy sitting at DC, which is the centre of the published span.
       It is an artifact of the receiver and not a signal in the air, so it
       must not draw as a carrier down the middle of the waterfall. */
    p25_spectrum_init();
    p25_spectrum_enable(true);
    make_tone(0);
    LS_EQ_INT(P25_SPECTRUM_FEED_PUBLISHED, feed_n(8, false, 1000));

    static float bins[P25_SPECTRUM_BINS];
    p25_spectrum_snapshot_t snap;
    LS_CHECK(p25_spectrum_read(bins, P25_SPECTRUM_BINS, 1000, 500, &snap));

    const int centre = P25_SPECTRUM_BINS / 2;
    float away = 0.0f;
    for (int i = 0; i < P25_SPECTRUM_BINS; i++)
        if (i < centre - 8 || i > centre + 8) away += bins[i];
    away /= (float)(P25_SPECTRUM_BINS - 17);

    /* Unnotched the spur reads full scale. The notch trades total removal for
       keeping a real carrier's centre, so the bar is that it stops being a
       saturated line, not that it reaches the floor. */
    for (int i = centre - 2; i <= centre + 2; i++)
        LS_CHECK_MSG(bins[i] <= 0.20f,
                     "bin %d reads %.3f of full scale against a %.3f floor - "
                     "the spur is still a line down the middle", i,
                     (double)bins[i], (double)away);
}

LS_CASE(a_real_carrier_off_centre_survives_the_notch)
{
    /* The notch is three bins wide and interpolates across them. Anything
       the receiver actually heard must be untouched, or the fix for the
       artifact has eaten the signal. */
    p25_spectrum_init();
    p25_spectrum_enable(true);
    make_tone(40);
    LS_EQ_INT(P25_SPECTRUM_FEED_PUBLISHED, feed_n(8, false, 2000));

    static float bins[P25_SPECTRUM_BINS];
    p25_spectrum_snapshot_t snap;
    LS_CHECK(p25_spectrum_read(bins, P25_SPECTRUM_BINS, 2000, 500, &snap));

    int peak = 0;
    for (int i = 1; i < P25_SPECTRUM_BINS; i++)
        if (bins[i] > bins[peak]) peak = i;
    const int centre = P25_SPECTRUM_BINS / 2;
    LS_CHECK_MSG(peak < centre - 1 || peak > centre + 1,
                 "the strongest bin landed at %d, inside the notch", peak);
    LS_CHECK_MSG(bins[peak] > 0.5f,
                 "the carrier only reads %.3f - the notch flattened it",
                 (double)bins[peak]);
}
