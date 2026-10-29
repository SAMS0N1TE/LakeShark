/* CARRIER LEVEL as the SD app builds it (app-sdk/apps/level): the level
   of the 12.5 kHz channel at the tuning, read every 2 ms from synthetic
   I/Q, the last whole second and the readout. */

#include "ls_test.h"
#include "meter.h"   /* app-sdk/apps/level */

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846
#define RATE 125000u
static int16_t iq[2 * 2500];

/* n pairs from sample `first`: a tone at f Hz of amplitude amp (of full
   scale), no noise. */
static void tone(uint64_t first, int n, double f, double amp)
{
    for (int k = 0; k < n; k++) {
        const double ph = 2 * PI * f * (double)(first + (uint64_t)k) / RATE;
        iq[2 * k] = (int16_t)lrint(amp * cos(ph) * 32767.0);
        iq[2 * k + 1] = (int16_t)lrint(amp * sin(ph) * 32767.0);
    }
}

LS_CASE(carrier_before_a_run_says_what_it_will_tune)
{
    static char out[8][METER_LINE];
    const int n = meter_lines(NULL, 162025000u, out, 8);
    LS_EQ_INT(n, 6);
    LS_EQ_STR(out[0], "FREQ    162.0250 MHz");
    LS_EQ_STR(out[1], "NOW     no reading yet");
    LS_EQ_STR(out[2], "1 SEC   --");
    LS_EQ_STR(out[4], "-100 [..............................] 0");
    LS_EQ_STR(out[5], "READS   0  gaps 0");
}

LS_CASE(carrier_reads_the_level_and_closes_each_second)
{
    static meter_t m;
    static char out[8][METER_LINE];
    meter_start(&m, 868300000u, RATE);
    /* Half a second at -40 dBFS, half a second at -20 (a steady level at
       0 Hz, which the channel filter passes whole); the second closes on
       the first reading a second after the first. */
    uint64_t first = 0;
    for (; first < RATE / 2; first += 2500) { tone(first, 2500, 0, 0.01); meter_feed(&m, iq, 2500, first, false); }
    int n = meter_lines(&m, 0, out, 8);
    LS_EQ_STR(out[0], "FREQ    868.3000 MHz");
    LS_EQ_STR(out[1], "NOW      -40.0 dBFS");
    LS_EQ_STR(out[2], "1 SEC   --");
    for (; first < RATE + 2500; first += 2500) { tone(first, 2500, 0, 0.1); meter_feed(&m, iq, 2500, first, false); }
    n = meter_lines(&m, 0, out, 8);
    LS_EQ_INT(n, 6);
    LS_EQ_STR(out[1], "NOW      -20.0 dBFS");
    LS_EQ_STR(out[2], "1 SEC   avg  -30.0 min  -40.0 max  -20.0");
    LS_CHECK(strstr(out[3], "START   avg ") == out[3]);
    LS_EQ_STR(out[4], "-100 [########################......] 0");
    LS_EQ_STR(out[5], "READS   510  gaps 0");
    /* A gap is counted. */
    meter_feed(&m, iq, 100, first + 100000, true);
    n = meter_lines(&m, 0, out, 8);
    LS_EQ_STR(out[5], "READS   510  gaps 1");
}

LS_CASE(carrier_channel_is_narrow)
{
    static meter_t m;
    meter_start(&m, 915000000u, RATE);
    /* 25 kHz off the tuning, on the filter's second null: far below. */
    for (uint64_t first = 0; first < 20000; first += 2000) { tone(first, 2000, 25000, 0.1); meter_feed(&m, iq, 2000, first, false); }
    LS_CHECK_MSG(m.now < -50.0f, "%.1f", (double)m.now);
}
