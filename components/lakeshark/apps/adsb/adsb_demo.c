/* See adsb_demo.h. */

#include "adsb_demo.h"
#include "adsb_state.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "adsb_demo";

#define DEMO_MAX      8
#define DEMO_ICAO_BASE 0xF00000UL   /* unassigned; cannot collide with a real one */

static int   s_n = 0;
static float s_lat = 43.2081f;      /* the ZeroMesh default, so the two agree */
static float s_lon = -71.5376f;

void adsb_demo_center(float lat, float lon)
{
    s_lat = lat;
    s_lon = lon;
}

int adsb_demo_count(void) { return s_n; }

void adsb_demo_set(int n)
{
    if (n < 0) n = 0;
    if (n > DEMO_MAX) n = DEMO_MAX;

    /* Retire any that are going away, so turning the demo down does not leave
       ghosts on the map that never age out. */
    for (int i = n; i < DEMO_MAX; i++) {
        adsb_aircraft_t *a = adsb_state_find_or_create(DEMO_ICAO_BASE + i);
        if (a) memset(a, 0, sizeof(*a));
    }

    s_n = n;
    ESP_LOGW(TAG, "%d demo aircraft%s", n,
             n ? " - these are SYNTHETIC, not received" : " (off)");
    if (n) adsb_demo_tick();
}

void adsb_demo_tick(void)
{
    if (s_n <= 0) return;

    double t = (double)esp_timer_get_time() / 1000000.0;

    for (int i = 0; i < s_n; i++) {
        adsb_aircraft_t *a = adsb_state_find_or_create(DEMO_ICAO_BASE + i);
        if (!a) continue;

        double radius_deg = 0.05 + 0.03 * i;          /* ~5 to ~30 km out */
        /* An airliner's speed or less: 180 kt and up, so the orbit is one a
           real aircraft could fly. A degree of arc is 60 nm. */
        const double kt   = 180.0 + 50.0 * i;
        double rate       = kt / (radius_deg * 60.0 * 3600.0);   /* radians per second */
        double ang        = t * rate + (double)i;

        a->icao      = (uint32_t)(DEMO_ICAO_BASE + i);
        snprintf(a->callsign, sizeof(a->callsign), "DEMO%02d", i + 1);
        a->lat       = s_lat + (float)(radius_deg * sin(ang));
        /* Longitude degrees shrink with latitude; without this the orbits are
           visibly stretched east-west on any map that is not at the equator. */
        a->lon       = s_lon + (float)(radius_deg * cos(ang) /
                                       cos((double)s_lat * M_PI / 180.0));
        a->pos_valid = true;
        a->pos_ts_us = esp_timer_get_time();

        a->altitude  = 3000 + i * 1500;
        /* The speed the orbit actually flies, so a map that dead reckons
           between reports lands where the next report does. */
        const double north_deg_s = radius_deg * rate * cos(ang);
        const double east_deg_s  = -radius_deg * rate * sin(ang);
        const double kt_per_deg_s = 60.0 * 3600.0;
        a->ns_velocity = (int)lround(north_deg_s * kt_per_deg_s);
        a->ew_velocity = (int)lround(east_deg_s * kt_per_deg_s);
        a->velocity  = (int)lround(hypot(north_deg_s, east_deg_s) * kt_per_deg_s);
        /* Heading is the direction of travel, in compass degrees. */
        a->heading   = (int)lround(fmod(atan2(east_deg_s, north_deg_s) * 180.0 / M_PI + 360.0, 360.0)) % 360;
        a->vert_rate = (i % 3 == 0) ? 0 : (i % 3 == 1) ? 900 : -700;
        adsb_state_push_position(a);
        a->msg_count += 1;
        a->last_seen_us = esp_timer_get_time();
        a->active    = true;
    }
}
