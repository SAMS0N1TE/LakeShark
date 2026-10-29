/* LS_TEST_SOURCES: */
/* The HackRF's slow rates end to end, the way hackrf_dev.c runs them: the
   radio tuned shift output-rates high, its own carrier leak on the wire,
   signed bytes in reads of whatever size the USB ring hands over, the CIC
   with the mixer and the AGC on, offset-binary out. A tone a known distance
   from the wanted centre must come out at that distance, with its sign,
   loud, and nothing else near it. */
#include "ls_test.h"
#include "hackrf_radio.h"

#include <stdlib.h>
#include <string.h>

/* Power of the output at `hz` (output rate `fs`), by correlation. */
static double power_at(const uint8_t *out, size_t pairs, double hz, double fs)
{
    double re = 0, im = 0;
    for (size_t n = 0; n < pairs; ++n) {
        const double i = out[2 * n] - 128.0, q = out[2 * n + 1] - 128.0;
        const double p = -2 * M_PI * hz * (double)n / fs;
        re += i * cos(p) - q * sin(p);
        im += i * sin(p) + q * cos(p);
    }
    return (re * re + im * im) / ((double)pairs * pairs);
}

typedef struct {
    double amp_in;     /* tone amplitude on the wire, in steps */
    double leak;       /* the radio's own carrier, DC on the wire */
    double offset_hz;  /* the tone, from the wanted centre */
} scene_t;

/* Runs `seconds` of the scene through the driver's path at output rate
   `rate`; answers the output pairs written to `out` (at most `cap`). */
static size_t run_scene(uint32_t rate, const scene_t *s, double seconds,
                        uint8_t *out, size_t cap)
{
    const unsigned decim = ls_hackrf_decimation(rate);
    const double hw = (double)rate * decim;
    const double shift_hz = (double)ls_hackrf_shift(decim) * rate;
    const size_t n_in = (size_t)(hw * seconds);
    int8_t *raw = malloc(2 * n_in);
    uint32_t seed = 12345;
    for (size_t n = 0; n < n_in; ++n) {
        /* The radio sits shift_hz above the centre, so the tone arrives at
           offset - shift on the wire; the leak is the radio's own DC. */
        const double p = 2 * M_PI * (s->offset_hz - shift_hz) * (double)n / hw;
        seed = seed * 1103515245u + 12345u;
        const double d1 = ((seed >> 16) & 0xff) / 256.0 - 0.5;
        seed = seed * 1103515245u + 12345u;
        const double d2 = ((seed >> 16) & 0xff) / 256.0 - 0.5;
        long i = lround(s->amp_in * cos(p) + s->leak + d1);
        long q = lround(s->amp_in * sin(p) + d2);
        raw[2 * n] = (int8_t)(i < -128 ? -128 : i > 127 ? 127 : i);
        raw[2 * n + 1] = (int8_t)(q < -128 ? -128 : q > 127 ? 127 : q);
    }
    ls_hackrf_cic_t c;
    ls_hackrf_cic_start(&c, decim, 0);   /* as hackrf_iq_configure does */
    /* The driver asks the ring for bytes * decim and takes what is there:
       uneven, sometimes odd, counts. */
    static const size_t READS[] = { 32768, 4097, 16384, 1, 777, 32768, 9 };
    size_t at = 0, made = 0;
    for (int r = 0; at < 2 * n_in; ++r) {
        size_t step = READS[r % 7];
        if (step > 2 * n_in - at) step = 2 * n_in - at;
        if (made + step / decim + 2 > 2 * cap) break;
        made += ls_hackrf_cic_run(&c, (const uint8_t *)raw + at, step, out + made);
        at += step;
    }
    free(raw);
    return made / 2;
}

static void check_scene(uint32_t rate, double offset_hz, double amp, double leak)
{
    enum { CAP = 400000 };
    static uint8_t out[2 * CAP];
    const scene_t s = { amp, leak, offset_hz };
    const size_t pairs = run_scene(rate, &s, rate > 600000 ? 0.2 : 0.5, out, CAP);
    LS_CHECK_MSG(pairs > rate / 6, "%lu S/s: only %zu pairs came out", (unsigned long)rate, pairs);
    /* Judge the second half, after the AGC has settled. */
    const uint8_t *tail = out + 2 * (pairs / 2);
    const size_t n = pairs - pairs / 2;
    double sum = 0, sq = 0;
    for (size_t k = 0; k < 2 * n; ++k) { const double v = tail[k] - 128.0; sum += v; sq += v * v; }
    const double rms = sqrt(sq / (2.0 * n));
    const double want = power_at(tail, n, offset_hz, rate);
    const double mirror = power_at(tail, n, -offset_hz, rate);
    const double dc = power_at(tail, n, 0, rate);
    LS_CHECK_MSG(rms > 16, "%lu S/s: flat output, rms %.2f steps", (unsigned long)rate, rms);
    LS_CHECK_MSG(sqrt(want) > 20, "%lu S/s: tone at %+.0f Hz came out at %.1f steps",
                 (unsigned long)rate, offset_hz, sqrt(want));
    LS_CHECK_MSG(want > 30 * mirror, "%lu S/s: tone mirrored (%.1f vs %.1f)",
                 (unsigned long)rate, sqrt(want), sqrt(mirror));
    LS_CHECK_MSG(want > 30 * dc, "%lu S/s: carrier leak came through (%.1f vs tone %.1f)",
                 (unsigned long)rate, sqrt(dc), sqrt(want));
}

/* A weak tone: three steps on the wire, under a leak twice its size. */
static void check_rate(uint32_t rate, double offset_hz) { check_scene(rate, offset_hz, 3.0, 6.0); }

LS_CASE(every_slow_rate_an_app_uses_delivers_the_tone_at_its_offset)
{
    /* P25, pagers and REC (240-256k), FM wide (516096), cell (1.92M), and
       the ends of the decimator's range. */
    static const uint32_t RATES[] = {
        100000, 125000, 240000, 245760, 256000, 500000, 516096,
        960000, 1024000, 1920000,
    };
    for (size_t i = 0; i < sizeof(RATES) / sizeof(RATES[0]); ++i) {
        check_rate(RATES[i], RATES[i] / 16.0);
        check_rate(RATES[i], -(double)RATES[i] / 10.0);
    }
    /* The cell monitor's 1.92 MS/s runs at factor 2, no room to shift: a
       one-step signal under a leak twenty steps tall still fills the byte. */
    check_scene(1920000, 300000, 1.0, 20.0);
}

LS_CASE(saved_gains_on_the_rtl_scale_reach_the_hackrf_twice_as_high)
{
    /* Up to 49.6 dB a setting is on the RTL-SDR's scale and the HackRF
       gets about twice it; above, it climbs to the HackRF's top. */
    LS_EQ_INT(ls_hackrf_gain_from_setting(0), 0);
    LS_EQ_INT(ls_hackrf_gain_from_setting(297), 594);
    LS_EQ_INT(ls_hackrf_gain_from_setting(496), 992);
    LS_EQ_INT(ls_hackrf_gain_from_setting(1130), 1130);
    int last = -1;
    for (int t = 0; t <= 1130; ++t) {
        const int g = ls_hackrf_gain_from_setting(t);
        LS_CHECK_MSG(g >= last && g <= 1130, "setting %d -> %d after %d", t, g, last);
        last = g;
    }
    LS_EQ_INT(ls_hackrf_gain_from_setting(320), 640);   /* cell_iq's HackRF capture */
}
