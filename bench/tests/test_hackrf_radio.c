/* LS_TEST_SOURCES: */
#include "ls_test.h"
#include "hackrf_radio.h"

#include <string.h>

LS_CASE(m0_state_wire_layout_uses_40_bytes_and_preserves_failure_evidence)
{
    const uint8_t wire[40] = {
        2,0,1,0, 2,0,0,0, 0x78,0x56,0x34,0x12, 0xf0,0xde,0xbc,0x9a,
        7,0,0,0, 0,0x20,0,0, 0,0,0x10,0, 0xff,0xff,0xff,0xff,
        1,0,0,0, 3,0,0,0
    };
    ls_radio_iq_health_t h;
    LS_CHECK(ls_hackrf_decode_m0_state(wire,sizeof(wire),&h));
    LS_EQ_UINT(h.requested_mode,2);LS_EQ_UINT(h.request_flag,1);
    LS_EQ_UINT(h.active_mode,2);LS_EQ_UINT(h.m0_count,0x12345678);
    LS_EQ_UINT(h.m4_count,0x9abcdef0);LS_EQ_UINT(h.num_shortfalls,7);
    LS_EQ_UINT(h.longest_shortfall,8192);LS_EQ_UINT(h.shortfall_limit,1048576);
    LS_EQ_UINT(h.threshold,UINT32_MAX);LS_EQ_UINT(h.next_mode,1);LS_EQ_UINT(h.error,3);
    LS_CHECK(!ls_hackrf_decode_m0_state(wire,39,&h));
    LS_EQ_UINT(h.num_shortfalls,0);LS_EQ_UINT(h.m0_count,0);
    LS_CHECK(!ls_hackrf_decode_m0_state(NULL,40,&h));
}

LS_CASE(signed_iq_is_converted_to_rtl_style_offset_binary)
{
    uint8_t samples[] = {
        UINT8_C(0x80), /* -128 */
        UINT8_C(0xff), /*   -1 */
        UINT8_C(0x00), /*    0 */
        UINT8_C(0x01), /*    1 */
        UINT8_C(0x7f), /*  127 */
    };
    const uint8_t expected[] = {0, 127, 128, 129, 255};
    ls_hackrf_iq_s8_to_u8(samples, sizeof(samples));
    LS_CHECK(memcmp(samples, expected, sizeof(samples)) == 0);
}

LS_CASE(endpoint_profile_is_receive_only_half_duplex_and_honest)
{
    LS_EQ_UINT(LS_HACKRF_CAPABILITIES, LS_RADIO_RX_IQ_U8);
    LS_CHECK((LS_HACKRF_CAPABILITIES & LS_RADIO_TX_PACKET) == 0);
    LS_EQ_INT(LS_HACKRF_DUPLEX, LS_RADIO_DUPLEX_HALF);
    LS_CHECK(ls_hackrf_frequency_ranges[0].min_hz == UINT64_C(1000000));
    LS_CHECK(ls_hackrf_frequency_ranges[0].max_hz == UINT64_C(6000000000));
    LS_EQ_UINT(ls_hackrf_sample_rate_ranges[0].min_hz, UINT64_C(100000));
    LS_EQ_UINT(ls_hackrf_sample_rate_ranges[0].max_hz, UINT64_C(20000000));
    LS_EQ_UINT(LS_HACKRF_MIN_STREAM_BYTES_PER_SEC, UINT32_C(4000000));
}

LS_CASE(vendor_payloads_are_little_endian)
{
    uint8_t payload[8];
    const uint8_t frequency_expected[8] = {
        0x70, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    ls_hackrf_encode_frequency(UINT64_C(6000000000), payload);
    LS_CHECK(memcmp(payload, frequency_expected, sizeof(payload)) == 0);

    const uint8_t rate_expected[8] = {
        0x80, 0x84, 0x1e, 0x00, 0x01, 0x00, 0x00, 0x00,
    };
    ls_hackrf_encode_sample_rate(UINT32_C(2000000), payload);
    LS_CHECK(memcmp(payload, rate_expected, sizeof(payload)) == 0);
    LS_EQ_UINT(ls_hackrf_filter_bandwidth(0, 2000000), 1750000);
}

#include <math.h>

LS_CASE(slow_rates_run_the_radio_at_a_whole_multiple)
{
    LS_EQ_UINT(ls_hackrf_decimation(256000), 8);    /* 2.048 MSPS */
    LS_EQ_UINT(ls_hackrf_decimation(240000), 9);    /* 2.16 MSPS  */
    LS_EQ_UINT(ls_hackrf_decimation(1920000), 2);   /* 3.84 MSPS  */
    LS_EQ_UINT(ls_hackrf_decimation(2000000), 1);
    LS_EQ_UINT(ls_hackrf_decimation(20000000), 1);
    LS_EQ_UINT(ls_hackrf_decimation(20000001), 0);
    LS_EQ_UINT(ls_hackrf_decimation(50000), 0);     /* would need 40 */
    LS_EQ_UINT(ls_hackrf_decimation(0), 0);
    LS_CHECK(ls_hackrf_sample_rate_ranges[0].min_hz <= 240000);
}

/* A complex tone at `hz` through the CIC at 256 kSPS out; answers the output
   amplitude, in offset-binary steps. */
static double tone_through(double hz, unsigned factor, double in_rate)
{
    enum { N = 64000 };
    static uint8_t in[2 * N], out[2 * N];
    for (int n = 0; n < N; ++n) {
        const double p = 2 * M_PI * hz * n / in_rate;
        in[2 * n]     = (uint8_t)(int8_t)lround(20 * cos(p));
        in[2 * n + 1] = (uint8_t)(int8_t)lround(20 * sin(p));
    }
    ls_hackrf_cic_t c;
    ls_hackrf_cic_init(&c, factor, 0);
    const size_t m = ls_hackrf_cic_run(&c, in, sizeof(in), out);
    double sq = 0;
    int count = 0;
    for (size_t k = 200; k + 1 < m; k += 2, ++count) {
        const double i = out[k] - 128.0, q = out[k + 1] - 128.0;
        sq += i * i + q * q;
    }
    return count ? sqrt(sq / count) : 0;
}

LS_CASE(cic_passes_the_band_and_nulls_the_aliases)
{
    /* DC and a tone well inside the 256 kSPS band come through at about the
       level they went in times factor/2; a tone at the output rate, which would alias onto
       DC, is gone. */
    const double in_rate = 2048000;
    const double dc = tone_through(0, 8, in_rate);
    LS_CHECK_MSG(fabs(dc - 80) < 2, "DC came out at %.1f", dc);   /* 20 in, x4 */
    const double pass = tone_through(20000, 8, in_rate);
    LS_CHECK_MSG(pass > 72, "20 kHz came out at %.1f", pass);
    const double alias = tone_through(256000, 8, in_rate);
    LS_CHECK_MSG(alias < 2, "256 kHz aliased in at %.1f", alias);
    const double alias2 = tone_through(512000 + 5000, 8, in_rate);
    LS_CHECK_MSG(alias2 < 3, "517 kHz aliased in at %.1f", alias2);
}

LS_CASE(cic_reads_split_anywhere_give_the_same_samples)
{
    enum { N = 9000 };
    static uint8_t in[2 * N], whole[2 * N], split[2 * N];
    for (int n = 0; n < 2 * N; ++n) in[n] = (uint8_t)(n * 37 + (n >> 3));
    ls_hackrf_cic_t a, b;
    ls_hackrf_cic_init(&a, 9, 2);
    ls_hackrf_cic_init(&b, 9, 2);
    const size_t m = ls_hackrf_cic_run(&a, in, sizeof(in), whole);
    size_t got = 0, at = 0;
    static const size_t STEPS[] = { 1, 7, 2, 513, 3, 1024, 5 };
    for (int s = 0; at < sizeof(in); ++s) {
        size_t step = STEPS[s % 7];
        if (at + step > sizeof(in)) step = sizeof(in) - at;
        got += ls_hackrf_cic_run(&b, in + at, step, split + got);
        at += step;
    }
    LS_EQ_UINT(got, m);
    LS_CHECK(memcmp(whole, split, m) == 0);
    LS_EQ_UINT(m, 2 * (N / 9));
}

/* Mean of the output after it settles, as I and Q in offset-binary steps. */
static void shifted_mean(double hz, double dc_i, unsigned factor, double in_rate,
                         double *mi, double *mq)
{
    enum { N = 64000 };
    static uint8_t in[2 * N], out[2 * N];
    for (int n = 0; n < N; ++n) {
        const double p = 2 * M_PI * hz * n / in_rate;
        in[2 * n]     = (uint8_t)(int8_t)lround(10 * cos(p) + dc_i);
        in[2 * n + 1] = (uint8_t)(int8_t)lround(10 * sin(p));
    }
    ls_hackrf_cic_t c;
    ls_hackrf_cic_init(&c, factor, ls_hackrf_shift(factor));
    const size_t m = ls_hackrf_cic_run(&c, in, sizeof(in), out);
    double si = 0, sq = 0;
    int count = 0;
    for (size_t k = 200; k + 1 < m; k += 2, ++count) { si += out[k] - 128.0; sq += out[k + 1] - 128.0; }
    *mi = si / count;
    *mq = sq / count;
}

LS_CASE(the_radio_is_tuned_aside_and_its_own_carrier_is_nulled)
{
    /* At 240 kSPS the radio runs at 2.16 MSPS tuned 480 kHz high, so the
       channel arrives at -480 kHz. The mixer brings it to the centre; the
       radio's carrier leak, DC on the wire, lands on a CIC null. */
    LS_EQ_UINT(ls_hackrf_shift(9), 2);
    LS_EQ_UINT(ls_hackrf_shift(8), 2);
    LS_EQ_UINT(ls_hackrf_shift(3), 1);
    LS_EQ_UINT(ls_hackrf_shift(2), 0);
    double mi, mq;
    shifted_mean(-480000, 0, 9, 2160000, &mi, &mq);
    LS_CHECK_MSG(fabs(mi - 45) < 3 && fabs(mq) < 3, "channel came out at %.1f,%.1f", mi, mq);
    shifted_mean(-480000 + 1e6, 20, 9, 2160000, &mi, &mq);   /* only the leak in band */
    LS_CHECK_MSG(fabs(mi) < 1.5 && fabs(mq) < 1.5, "carrier leak came through at %.1f,%.1f", mi, mq);
}

LS_CASE(the_agc_fills_the_byte_for_a_weak_carrier_and_backs_off_a_strong_one)
{
    /* A carrier one step tall in, factor 8: without the AGC it comes out
       four steps tall; with it, after it settles, between 32 and 96. */
    enum { N = 200000 };
    static uint8_t in[2 * N], out[2 * N];
    const double in_rate = 2048000;
    for (int amp = 1; amp <= 100; amp += 99) {
        for (int n = 0; n < N; ++n) {
            const double p = 2 * M_PI * -512000.0 * n / in_rate + 2 * M_PI * 3000.0 * n / in_rate;
            in[2 * n]     = (uint8_t)(int8_t)lround(amp * cos(p));
            in[2 * n + 1] = (uint8_t)(int8_t)lround(amp * sin(p));
        }
        ls_hackrf_cic_t c;
        ls_hackrf_cic_init(&c, 8, ls_hackrf_shift(8));
        c.agc = true;
        const size_t m = ls_hackrf_cic_run(&c, in, sizeof(in), out);
        int peak = 0, clipped = 0;
        for (size_t k = m / 2; k < m; ++k) {
            const int v = out[k] - 128;
            if ((v < 0 ? -v : v) > peak) peak = v < 0 ? -v : v;
            clipped += out[k] == 0 || out[k] == 255;
        }
        LS_CHECK_MSG(peak >= 28 && peak <= 110, "amplitude %d came out peaking at %d", amp, peak);
        LS_CHECK_MSG(clipped == 0, "amplitude %d clipped %d samples", amp, clipped);
    }
}
