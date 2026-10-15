/* Aircraft source for DETECTOR: reads the shared ADS-B/UAT table that another
 * app's receiver fills. It claims no radio and starts nothing. */
#include "ls_detector_air.h"
#include "apps/adsb/adsb_app.h"
#include "apps/adsb/adsb_state.h"
#include "ls_gps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

_Static_assert(ADSB_MAX_TRACKED <= LS_DETECTOR_AIR_SLOTS, "the pass must cover the aircraft table");

const char *ls_detector_air_source(void) {
    switch (adsb_active_source()) {
    case ADSB_SRC_IQ: return "IQ";
    case ADSB_SRC_LORA: return "LR chip";
    case ADSB_SRC_DUAL: return "LR+RTL";
    case ADSB_SRC_UAT: return "UAT";
    default: return NULL;
    }
}

void ls_detector_air_gps(ls_detector_air_gps_t *out, int64_t now) {
    ls_gps_state_t g;
    ls_gps_get(&g);
    out->fix = ls_gps_fix_current(&g, now);
    out->lat = out->fix ? g.lat_deg : 0;
    out->lon = out->fix ? g.lon_deg : 0;
}

static double haversine_km(double lat1, double lon1, double lat2, double lon2) {
    const double rad = 3.14159265358979323846 / 180.0;
    double dlat = (lat2 - lat1) * rad, dlon = (lon2 - lon1) * rad;
    double s = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * rad) * cos(lat2 * rad) * sin(dlon / 2) * sin(dlon / 2);
    if (s > 1) s = 1;
    return 12742.0 * asin(sqrt(s));
}

static bool fresh(int64_t now, int64_t stamp, int64_t limit) {
    return stamp > 0 && now >= stamp && now - stamp <= limit;
}

bool ls_detector_air_read(int slot, int64_t now, const ls_detector_air_gps_t *gps, ls_detector_air_t *out) {
    const adsb_aircraft_t *a = adsb_state_get(slot);
    if (!a || !a->active || !fresh(now, a->last_seen_us, LS_DETECTOR_AIR_STALE_US)) return false;
    memset(out, 0, sizeof(*out));
    out->icao = a->icao;
    out->qualifier = a->address_qualifier;
    out->sources = a->sources;
    out->last_seen_us = a->last_seen_us;
    out->emitter_tc = a->emitter_tc;
    out->emitter_ca = a->emitter_ca;
    out->altitude_ft = INT32_MIN;
    out->distance_x10_km = LS_DETECTOR_AIR_NO_DISTANCE;
    /* The decoder writes the callsign from another task: copy it and keep only
     * the characters an identification can hold, so a torn read is empty. */
    char c[9];
    memcpy(c, a->callsign, sizeof(c));
    c[8] = 0;
    size_t n = 0;
    while (n < 8 && ((c[n] >= 'A' && c[n] <= 'Z') || (c[n] >= '0' && c[n] <= '9'))) n++;
    if (!c[n]) memcpy(out->callsign, c, n + 1);
    int64_t at = 0;
    int squawk = adsb_state_squawk_at(slot, &at);
    if (squawk > 0 && fresh(now, at, LS_DETECTOR_AIR_STALE_US)) out->squawk = (uint16_t)squawk;
    int state = adsb_state_emergency(slot, &at);
    if (state > 0 && fresh(now, at, LS_DETECTOR_AIR_STALE_US)) out->emergency = (uint8_t)state;
    if (a->altitude_valid && fresh(now, a->altitude_ts_us, ADSB_FIELD_STALE_US)) out->altitude_ft = a->altitude;
    if (gps && gps->fix && a->pos_valid && fresh(now, a->pos_ts_us, ADSB_FIELD_STALE_US) &&
        a->lat >= -90 && a->lat <= 90 && a->lon >= -180 && a->lon <= 180) {
        double km = haversine_km(gps->lat, gps->lon, a->lat, a->lon);
        out->distance_x10_km = km >= 6553.0 ? 65534u : (uint16_t)(km * 10.0 + 0.5);
    }
    return true;
}

static const char *const categories[16] = {
    "light", "small", "large", "high_vortex", "heavy", "high_performance", "rotorcraft",
    "glider", "lighter_than_air", "parachutist", "ultralight", "uav", "space",
    "emergency_vehicle", "service_vehicle", "obstacle"
};
const char *ls_detector_air_category_name(unsigned bit) { return bit < 16 ? categories[bit] : "?"; }
/* Type code 4 is set A, 3 is B, 2 is C; category 0 and reserved entries name nothing. */
int ls_detector_air_category_bit(unsigned tc, unsigned ca) {
    if (tc == 4 && ca >= 1 && ca <= 7) return (int)ca - 1;
    if (tc == 3 && ca >= 1 && ca <= 7 && ca != 5) return ca == 6 ? 11 : ca == 7 ? 12 : (int)ca + 6;
    if (tc == 2 && ca >= 1 && ca <= 5) return ca == 1 ? 13 : ca == 2 ? 14 : 15;
    return -1;
}

bool ls_detector_air_match(const ls_detector_rule_t *r, const ls_detector_air_t *a) {
    if (!r || !a || !r->air_mask) return false;
    if (r->air_mask & DET_AIR_SQUAWK) {
        bool hit = false;
        for (unsigned i = 0; i < r->air_squawks; i++) hit |= a->squawk && a->squawk == r->air_squawk[i];
        if (!hit) return false;
    }
    if ((r->air_mask & DET_AIR_EMERG) && !a->emergency) return false;
    if (r->air_mask & DET_AIR_CALL) {
        bool hit = false;
        for (unsigned i = 0; i < r->air_calls && a->callsign[0]; i++)
            hit |= !strncmp(a->callsign, r->air_call[i], strlen(r->air_call[i]));
        if (!hit) return false;
    }
    if (r->air_mask & DET_AIR_ICAO) {
        bool hit = false;
        /* Only ICAO-qualified addresses; other namespaces can alias them. */
        for (unsigned i = 0; i < r->air_icaos && a->qualifier == 0; i++)
            hit |= a->icao >= r->air_icao_lo[i] && a->icao <= r->air_icao_hi[i];
        if (!hit) return false;
    }
    if (r->air_mask & DET_AIR_CAT) {
        int bit = ls_detector_air_category_bit(a->emitter_tc, a->emitter_ca);
        if (bit < 0 || !(r->air_cats & (1u << bit))) return false;
    }
    if (r->air_mask & DET_AIR_KM) {
        if (a->distance_x10_km == LS_DETECTOR_AIR_NO_DISTANCE || a->distance_x10_km > r->air_km * 10u) return false;
    }
    if (r->air_mask & DET_AIR_ALT) {
        if (a->altitude_ft == INT32_MIN || a->altitude_ft >= r->air_alt_below_ft) return false;
    }
    return true;
}

static void text(char *out, size_t cap, uint32_t icao, const char *call, unsigned squawk, int emergency,
                 int32_t alt, unsigned dist) {
    char id[12], tail[48] = "";
    if (call[0]) snprintf(id, sizeof(id), "%s", call);
    else snprintf(id, sizeof(id), "%06X", (unsigned)icao);
    size_t n = 0;
    if (squawk) n += (size_t)snprintf(tail + n, sizeof(tail) - n, " sq%04u", squawk);
    if (emergency && n < sizeof(tail)) n += (size_t)snprintf(tail + n, sizeof(tail) - n, " EMG");
    if (alt != INT32_MIN && n < sizeof(tail)) n += (size_t)snprintf(tail + n, sizeof(tail) - n, " %dft", (int)alt);
    if (dist != LS_DETECTOR_AIR_NO_DISTANCE && n < sizeof(tail))
        snprintf(tail + n, sizeof(tail) - n, " %u.%ukm", dist / 10, dist % 10);
    snprintf(out, cap, "%s%s", id, tail);
}
void ls_detector_air_text(const ls_detector_air_t *a, char *out, size_t cap) {
    text(out, cap, a->icao, a->callsign, a->squawk, a->emergency, a->altitude_ft, a->distance_x10_km);
}
void ls_detector_air_event_text(const ls_detector_event_t *e, char *out, size_t cap) {
    text(out, cap, e->icao, e->callsign, e->squawk, e->emergency, e->altitude_ft, e->distance_x10_km);
}
bool ls_detector_air_alarm(unsigned squawk, unsigned emergency) {
    return emergency || squawk == 7500 || squawk == 7600 || squawk == 7700;
}
