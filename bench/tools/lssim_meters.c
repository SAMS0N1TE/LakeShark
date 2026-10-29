/* METERS in the simulator: a morning's worth of ERT meters, so the screen
   is drawn with what it shows on a street, and REC's ISM listener answered
   as a 912.6 MHz SDR session would. LSSIM_METERS_EMPTY=1 leaves the store
   empty for the idle face. */

#include "ls_meters.h"
#include "rec_ism.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_hz = 912600000u;

bool rec_ism_listen(const uint32_t *hz, int n, uint32_t dwell_ms)
{
    (void)dwell_ms;
    if (hz && n > 0) s_hz = hz[0];
    return true;
}

void rec_ism_listen_status(rec_ism_listen_t *out)
{
    memset(out, 0, sizeof(*out));
    out->on = true;
    out->hz = s_hz;
    out->bursts = 211;
    out->decoded = 64;
    out->plan_n = 1;
}

static void hear(int proto, const char *id, long long v, int64_t t)
{
    lr433_msg_t m;
    memset(&m, 0, sizeof(m));
    m.proto = (uint8_t)proto;
    m.channel = -1;
    m.battery_ok = -1;
    snprintf(m.id, sizeof(m.id), "%s", id);
    m.kpa = m.temp_c = m.humidity = m.rain_mm = m.wind_ms = NAN;
    m.consumption = v;
    ls_meters_inject(&m, 912600000u, NAN, 0, t);
}

void lssim_meters_seed(void)
{
    if (getenv("LSSIM_METERS_EMPTY")) return;
    const int64_t now = ls_meters_now_s();
    /* The house: a morning load, kettle and oven spikes, readings every
       few minutes. */
    long long v = 562180;
    for (int k = 0; k <= 72; k++) {
        const int64_t t = now - 6 * 3600 + k * 300;
        const int hour = k / 12;
        v += hour < 2 ? 1 : hour < 4 ? 3 + (k % 5 == 0 ? 9 : 0) : 2 + (k % 7 == 0 ? 14 : 0);
        hear(LR433_P_ERT_SCM, "54585868", v, k == 72 ? now - 12 : t);
    }
    /* Gas: the heating comes on at dawn. */
    long long g = 339880;
    for (int k = 0; k <= 36; k++) {
        g += k > 10 && k < 24 ? 4 : k >= 24 ? 1 : 0;
        hear(LR433_P_ERT_IDM, "11278109", g, now - 6 * 3600 + k * 600 - 40);
    }
    /* A neighbour's newer meter, steady. */
    long long n = 10450;
    for (int k = 0; k <= 20; k++) {
        n += 2;
        hear(LR433_P_ERT_SCMPLUS, "68211547", n, now - 3 * 3600 + k * 540 - 95);
    }
    /* Once, a few minutes ago. */
    hear(LR433_P_ERT_SCM, "33002871", 9184412, now - 410);
    ls_meters_name(LR433_P_ERT_SCM, "54585868", "HOUSE");
    ls_meters_pin(LR433_P_ERT_SCM, "54585868", true);
    ls_meters_name(LR433_P_ERT_IDM, "11278109", "GAS");
}
