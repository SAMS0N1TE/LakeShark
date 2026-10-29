/* IRIDIUM BURSTS as the SD app builds it (app-sdk/apps/iridium): a radio
   at 125 kHz that hears 8 ms bursts every 90 ms on one simplex channel,
   which moves two channels over after fifteen seconds, as a pass's Doppler
   shift would carry it. The hop plan, blocks from the last channel passed
   over, burst counting per channel, and the frequency trace. */

#include "ls_test.h"
#include "irid.h"   /* app-sdk/apps/iridium, with ../burst/burst_det.h */

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846
#define RATE 125000u
static int16_t iq[2 * 2048];
static ls_rng_t rng;

static double gauss(void)
{
    double u1 = ((ls_rng_u32(&rng) >> 8) + 1.0) / 16777217.0;
    double u2 = (ls_rng_u32(&rng) >> 8) / 16777216.0;
    return sqrt(-2.0 * log(u1)) * cos(2 * PI * u2);
}

static uint32_t s_sat_a, s_sat_b;
static uint64_t s_switch;           /* sample number the satellite moves at */

/* A block at sample `first` heard by a radio tuned to `center`: the
   satellite's tone where it falls in the passband, in noise at -60 dBFS. */
static void air(uint64_t first, uint32_t center)
{
    const double sigma = sqrt(pow(10.0, -60 / 10.0) / 2.0);
    for (int k = 0; k < 2048; k++) {
        const uint64_t t = first + (uint64_t)k;
        const uint32_t sat = t < s_switch ? s_sat_a : s_sat_b;
        const bool on = (t * 1000000u / RATE) % 90000 < 8000;
        const double f = (double)sat - (double)center, ph = 2 * PI * f * (double)t / RATE;
        /* The radio's own filter keeps what is outside its rate out. */
        const double a = on && fabs(f) < RATE / 2 ? 0.03 : 0.0;
        iq[2 * k] = (int16_t)lrint((a * cos(ph) + sigma * gauss()) * 32768.0);
        iq[2 * k + 1] = (int16_t)lrint((a * sin(ph) + sigma * gauss()) * 32768.0);
    }
}

static const char *find_line(char (*out)[IRID_LINE], int n, const char *want)
{
    for (int i = 0; i < n; i++) if (strstr(out[i], want)) return out[i];
    return NULL;
}

LS_CASE(iridium_before_a_run_says_its_plan)
{
    static char out[4][IRID_LINE];
    LS_EQ_INT(irid_idle_lines(0, 90, 8, out, 4), 2);
    LS_EQ_STR(out[0], "SIMPLEX 1626.0-1626.5 MHz in 12 channels");
    LS_EQ_STR(out[1], "90 ms on each, bursts +8 dB over the floor");
}

LS_CASE(iridium_hops_the_simplex_channels_and_traces_the_strongest)
{
    static irid_t e;
    ls_rng_seed(&rng, 21);
    irid_start(&e, 0, 90, 8, RATE);
    /* Channel 6 is the ring alert channel, 1626.270833 MHz; the satellite
       drifts to channel 8 after fifteen seconds. */
    s_sat_a = 1626000000u + 41666u / 2 + 41666u * 6;
    s_sat_b = 1626000000u + 41666u / 2 + 41666u * 8;
    s_switch = (uint64_t)RATE * 15;
    uint32_t center = irid_ch_hz(&e, 0);
    LS_EQ_UINT(center, 1626020833u);
    int stale = 0;
    for (uint64_t first = 0; first < (uint64_t)RATE * 21; first += 2048) {
        air(first, center);
        if (irid_feed(&e, iq, 2048, first, center, false)) {
            /* The radio takes a block to retune: the next is still the old
               channel's, and is passed over. */
            const uint32_t next = irid_hop(&e);
            first += 2048;
            air(first, center);
            LS_CHECK(!irid_feed(&e, iq, 2048, first, center, false));
            stale++;
            center = next;
        }
    }
    LS_CHECK(e.hops > 150);
    LS_EQ_INT((int)e.hops, stale);
    static char out[IRID_LINES][IRID_LINE];
    const int n = irid_lines(&e, out, IRID_LINES);
    for (int i = 0; i < n; i++) printf("  | %s\n", out[i]);   /* the readout, for the log */
    for (int i = 0; i < n; i++) LS_CHECK_MSG(strlen(out[i]) <= 48, "line %d too long: %s", i, out[i]);
    LS_EQ_STR(out[0], "SIMPLEX 1626.0-1626.5 MHz, 12 x 41.7 kHz");
    LS_CHECK(strstr(out[1], "DWELL 90 ms  CYCLE ") == out[1]);
    LS_CHECK(strstr(out[1], "+8 dB") != NULL);
    LS_CHECK(strstr(out[2], "BURSTS ") == out[2]);
    LS_CHECK(strstr(out[2], "PEAK -30 dBFS") != NULL);
    const char *best = find_line(out, n, "BEST ");
    LS_CHECK(best && (strstr(best, "1626.271") || strstr(best, "1626.354")));
    /* Every burst on the two channels the satellite used, none elsewhere. */
    const char *ch = find_line(out, n, "CH [");
    LS_CHECK(ch != NULL);
    if (ch) {
        for (int c = 0; c < 12; c++) {
            const char mark = ch[4 + c];
            if (c == 6 || c == 8) LS_CHECK_MSG(mark != ' ' && mark != '.', "channel %d: '%c'", c, mark);
            else LS_CHECK_MSG(mark == ' ', "channel %d: '%c'", c, mark);
        }
    }
    LS_CHECK(find_line(out, n, "LEN ~") != NULL);
    /* The newest trace rows are on channel 8, older ones on channel 6. */
    int head = -1;
    for (int i = 0; i < n; i++) if (strstr(out[i], "TRACE") == out[i]) head = i;
    LS_CHECK(head >= 0 && head + 1 < n);
    if (head >= 0 && head + 1 < n) {
        LS_CHECK(strstr(out[head + 1], "1626.354") != NULL);
        LS_CHECK(strstr(out[head + 1], "........*...") != NULL);
        LS_CHECK(find_line(out, n, "......*.....") != NULL);   /* before the drift */
    }
}

LS_CASE(iridium_whole_band_plan_is_21_channels_of_500_khz)
{
    static irid_t e;
    irid_start(&e, 1, 180, 8, RATE);
    LS_EQ_UINT(irid_ch_hz(&e, 0), 1616250000u);
    static char out[4][IRID_LINE];
    irid_lines(&e, out, 4);
    LS_EQ_STR(out[0], "WHOLE BAND 1616.0-1626.5 MHz, 21 x 500.0 kHz");
    LS_CHECK(strstr(out[1], "DWELL 180 ms  CYCLE 3.8 s") == out[1]);
}
