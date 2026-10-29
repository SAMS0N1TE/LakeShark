#include "ls_rfsim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ls_gps.h"
#include "ls_textpack.h"

#define M_PER_DEG_LAT 111132.0

static double rad(double d) { return d * M_PI / 180.0; }

void ls_rfsim_walk_at(const ls_rfsim_walk_t *w, double t, double *lat, double *lon, float *course)
{
    const double leg = w->leg_m > 1 ? w->leg_m : 200, step = w->step_m > 1 ? w->step_m : 40;
    const double s = (t > 0 ? t : 0) * (w->speed_mps > 0 ? w->speed_mps : 1.4);
    const double period = 2 * (leg + step);
    const double k = floor(s / period), r = s - k * period;
    double along, across = k * 2 * step;
    float dir;
    if (r < leg)                 { along = r;                    dir = 0; }
    else if (r < leg + step)     { along = leg; across += r - leg; dir = 90; }
    else if (r < 2 * leg + step) { along = leg - (r - leg - step); across += step; dir = 180; }
    else                         { along = 0; across += step + (r - 2 * leg - step); dir = 90; }
    const double c = rad(w->course), m_lon = M_PER_DEG_LAT * cos(rad(w->lat));
    const double e = along * sin(c) + across * sin(c + M_PI / 2);
    const double n = along * cos(c) + across * cos(c + M_PI / 2);
    *lat = w->lat + n / M_PER_DEG_LAT;
    *lon = w->lon + e / (m_lon > 1 ? m_lon : 1);
    if (course) *course = fmodf(w->course + dir + 360.0f, 360.0f);
}

/* ---------------------------------------------------------------- state -- */

typedef struct { double lat, lon; float eirp_dbm; uint32_t hz; uint8_t duty_q; } tx_t;   /* duty in quarters */

static struct {
    ls_rfsim_mode_t mode;
    ls_rfsim_walk_t walk;
    int64_t start_us;
    bool placed, fox_set;
    tx_t tx[3];
} s;

static void place_world(double lat, double lon)
{
    /* Metres east and north of the start, so the walk passes near each. */
    static const float E[3] = { 120, -90, 260 }, N[3] = { 180, 300, 60 }, P[3] = { 0, -3, 3 };
    static const uint8_t DUTY[3] = { 4, 2, 1 };
    const double m_lon = M_PER_DEG_LAT * cos(rad(lat));
    for (int i = 0; i < 3; i++) {
        if (i == 0 && s.fox_set) continue;
        s.tx[i].lat = lat + N[i] / M_PER_DEG_LAT;
        s.tx[i].lon = lon + E[i] / m_lon;
        s.tx[i].eirp_dbm = P[i];
        s.tx[i].duty_q = DUTY[i];
    }
    s.placed = true;
}

void ls_rfsim_here(double lat, double lon)
{
    if (s.mode == LS_RFSIM_OFF || !s.placed) place_world(lat, lon);
    s.walk.lat = lat; s.walk.lon = lon;
    s.mode = LS_RFSIM_HERE;
}

void ls_rfsim_walk(double lat, double lon, float course, float speed)
{
    place_world(lat, lon);
    s.walk = (ls_rfsim_walk_t){ .lat = lat, .lon = lon, .course = course, .speed_mps = speed > 0 ? speed : 1.4f,
                                .leg_m = 220, .step_m = 25 };
    s.start_us = esp_timer_get_time();
    s.mode = LS_RFSIM_WALK;
}

void ls_rfsim_off(void) { s.mode = LS_RFSIM_OFF; }
ls_rfsim_mode_t ls_rfsim_mode(void) { return s.mode; }

void ls_rfsim_fox(double lat, double lon)
{
    s.tx[0].lat = lat; s.tx[0].lon = lon; s.tx[0].duty_q = 4; s.fox_set = true;
}

bool ls_rfsim_fox_at(double *lat, double *lon)
{
    if (!s.placed && !s.fox_set) return false;
    *lat = s.tx[0].lat; *lon = s.tx[0].lon;
    return true;
}

void ls_rfsim_plan(const uint32_t *hz, int n)
{
    static const int CH[3] = { 0, 2, 5 };
    for (int i = 0; i < 3; i++) s.tx[i].hz = hz && n > 0 ? hz[CH[i] % n] : 0;
}

bool ls_rf_where(double *lat, double *lon, uint16_t *flags, float *course)
{
    if (flags) *flags = 0;
    if (course) *course = NAN;
    if (s.mode == LS_RFSIM_HERE) {
        *lat = s.walk.lat; *lon = s.walk.lon;
        if (flags) *flags = LS_RF_WHERE_SIM;
        return true;
    }
    if (s.mode == LS_RFSIM_WALK) {
        float c;
        ls_rfsim_walk_at(&s.walk, (esp_timer_get_time() - s.start_us) / 1e6, lat, lon, &c);
        if (flags) *flags = LS_RF_WHERE_SIM;
        if (course) *course = c;
        return true;
    }
    /* The satellite table makes a GPS state too large for a stack, and the
       UI and the survey worker both ask: one copy, taken in turn. */
    EXT_RAM_BSS_ATTR static ls_gps_state_t g;
    static StaticSemaphore_t mem;
    static SemaphoreHandle_t m;
    static unsigned once;
    unsigned expect = 0;
    if (__atomic_compare_exchange_n(&once, &expect, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        m = xSemaphoreCreateMutexStatic(&mem);
        __atomic_store_n(&once, 2, __ATOMIC_RELEASE);
    } else while (__atomic_load_n(&once, __ATOMIC_ACQUIRE) != 2) vTaskDelay(1);
    xSemaphoreTake(m, portMAX_DELAY);
    ls_gps_get(&g);
    const bool fix = ls_gps_fix_current(&g, esp_timer_get_time());
    if (fix) {
        *lat = g.lat_deg; *lon = g.lon_deg;
        if (flags) *flags = LS_RF_WHERE_FIX;
        if (course && g.speed_kts > 1.0f) *course = g.course_deg;
    }
    xSemaphoreGive(m);
    return fix;
}

/* ------------------------------------------------------------- hearing -- */

static uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }

/* Repeatable fading: the same place at the same tenth of a second hears the
   same thing, so a render or a test does not flicker between runs. */
static float fade(uint32_t salt, int64_t now_us, double lat, double lon)
{
    const uint32_t h = mix(salt ^ (uint32_t)(now_us / 100000) ^ mix((uint32_t)(int32_t)(lat * 1e5)) ^ (uint32_t)(int32_t)(lon * 1e5) * 2654435761u);
    return ((h & 0xFFFF) / 65535.0f - 0.5f) * 3.0f;
}

static float heard(const tx_t *t, double lat, double lon, float heading, uint32_t hz)
{
    const double m_lon = M_PER_DEG_LAT * cos(rad(lat));
    const double e = (t->lon - lon) * m_lon, n = (t->lat - lat) * M_PER_DEG_LAT;
    double d = hypot(e, n);
    if (d < 5) d = 5;
    /* Free space at 1 m, then a third power with distance: a hand-held at
       head height among buildings and trees falls off faster than space. */
    const double f_mhz = (hz ? hz : 146000000u) / 1e6;
    float v = t->eirp_dbm - (float)(20 * log10(f_mhz) - 27.55 + 30 * log10(d));
    if (isfinite(heading)) {
        const double brg = atan2(e, n) * 180 / M_PI;
        v -= 14.0f * (1.0f - (float)cos(rad(heading - brg))) / 2.0f;
    }
    return v;
}

static float sum_db(float a, float b) { return 10.0f * log10f(powf(10.0f, a / 10) + powf(10.0f, b / 10)); }

float ls_rfsim_level(double lat, double lon, uint32_t hz, uint32_t bw, float heading, int64_t now_us)
{
    if (!s.placed) place_world(lat, lon);
    float v = -112.0f + fade(hz, now_us, lat, lon) * 0.5f;
    const uint32_t half = (bw ? bw : 12500) / 2;
    for (int i = 0; i < 3; i++) {
        const tx_t *t = &s.tx[i];
        if (!t->hz || t->hz + half < hz || t->hz > hz + half) continue;
        /* Keyed for duty_q quarters of every four seconds. */
        if ((uint32_t)((now_us / 1000000) % 4) >= t->duty_q) continue;
        v = sum_db(v, heard(t, lat, lon, heading, hz) + fade(i + 7, now_us, lat, lon));
    }
    return v;
}

float ls_rfsim_fox_level(double lat, double lon, float heading, int64_t now_us)
{
    if (!s.placed) place_world(lat, lon);
    return sum_db(-112.0f, heard(&s.tx[0], lat, lon, heading, s.tx[0].hz) + fade(3, now_us, lat, lon) * 0.6f);
}

/* ------------------------------------------------------------- console -- */

int ls_rfsim_command(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "here")) {
        ls_rfsim_here(atof(argv[2]), atof(argv[3]));
    } else if (argc >= 4 && !strcmp(argv[1], "walk")) {
        ls_rfsim_walk(atof(argv[2]), atof(argv[3]), argc > 4 ? (float)atof(argv[4]) : 0.0f, argc > 5 ? (float)atof(argv[5]) : 1.4f);
    } else if (argc >= 4 && !strcmp(argv[1], "fox")) {
        ls_rfsim_fox(atof(argv[2]), atof(argv[3]));
    } else if (argc == 2 && !strcmp(argv[1], "off")) {
        ls_rfsim_off();
    } else if (argc != 1) {
        printf("rfsim here LAT LON | walk LAT LON [COURSE] [M/S] | fox LAT LON | off\n");
        return 1;
    }
    double lat, lon; uint16_t fl; float c;
    if (ls_rf_where(&lat, &lon, &fl, &c))
        LS_PRINTF(t_ls_rfsim_rfsim_s_at_6f, "rfsim: %s at %.6f, %.6f%s", fl & LS_RF_WHERE_SIM ? (s.mode == LS_RFSIM_WALK ? "walking" : "standing") : "GPS fix",
               lat, lon, isfinite(c) ? "" : "\n");
    else printf("rfsim: off, and no GPS fix\n");
    if (isfinite(c)) printf(" course %.0f\n", c);
    if (s.placed)
        for (int i = 0; i < 3; i++)
            LS_PRINTF(t_ls_rfsim_tx_d_s_6f, "  tx%d %s %.6f, %.6f  %.4f MHz  %+.0f dBm  keyed %d/4\n", i, i ? "    " : "fox", s.tx[i].lat, s.tx[i].lon,
                   s.tx[i].hz / 1e6, s.tx[i].eirp_dbm, s.tx[i].duty_q);
    return 0;
}
