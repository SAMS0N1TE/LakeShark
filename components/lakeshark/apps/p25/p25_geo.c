/* See p25_geo.h. The decision, with no radio and no clock of its own. */

#include "p25_geo.h"

#include <math.h>

static const double DEG = 0.017453292519943295;   /* pi / 180 */
static const double EARTH_DIAMETER_M = 12742000.0;

double p25_geo_distance_m(double lat_a, double lon_a,
                          double lat_b, double lon_b)
{
    if (!isfinite(lat_a) || !isfinite(lon_a) ||
        !isfinite(lat_b) || !isfinite(lon_b))
        return INFINITY;

    /* Haversine, the same shape scan_geo uses for the conventional scanner.
       Deliberately the same: two different distance functions in one
       firmware is two different answers to "am I in range", and the one that
       is wrong is whichever you are not currently looking at. */
    const double a = lat_a * DEG, b = lat_b * DEG;
    const double dlat = (lat_b - lat_a) * DEG;
    const double dlon = (lon_b - lon_a) * DEG;
    double h = sin(dlat / 2) * sin(dlat / 2) +
               cos(a) * cos(b) * sin(dlon / 2) * sin(dlon / 2);
    /* asin is undefined past one, and rounding can put it there for
       antipodal points. */
    if (h < 0.0) h = 0.0;
    if (h > 1.0) h = 1.0;
    return EARTH_DIAMETER_M * asin(sqrt(h));
}

bool p25_geo_profile_has_sites(const p25_profile_t *profile)
{
    if (!profile) return false;
    for (unsigned i = 0; i < profile->control_count; ++i)
        if (profile->control_has_geo[i] &&
            profile->control_radius_m[i] >= P25_GEO_MIN_RADIUS_M)
            return true;
    return false;
}

bool p25_geo_ready(const p25_geo_t *state, int64_t now_us)
{
    return state && state->have_answer && now_us >= state->fix_us &&
           now_us - state->fix_us <= P25_GEO_HOLD_US;
}

int p25_geo_select(p25_geo_t *state, const p25_profile_t *profile,
                   bool valid, double lat, double lon,
                   int64_t fix_us, int64_t now_us)
{
    if (!state || !profile) return -1;
    if (!p25_geo_profile_has_sites(profile)) return -1;

    /* A fix has to be real, recent, and not from the future. The last of
       those is not paranoia: the clock is set from GPS, so the moment it
       first syncs, timestamps taken before it jump backwards relative to
       now, and a naive age check reads them as fresh forever. */
    if (!valid || !isfinite(lat) || !isfinite(lon) ||
        fabs(lat) > 90.0 || fabs(lon) > 180.0 ||
        fix_us <= 0 || fix_us > now_us ||
        now_us - fix_us > P25_GEO_FIX_MAX_AGE_US)
        return -1;

    const int held = p25_geo_ready(state, now_us) ? state->chosen : -1;

    int best = -1;
    double best_d = 0.0;
    double held_d = INFINITY;

    for (unsigned i = 0; i < profile->control_count; ++i) {
        if (!profile->control_has_geo[i]) continue;
        const uint32_t radius = profile->control_radius_m[i];
        if (radius < P25_GEO_MIN_RADIUS_M) continue;

        const double d = p25_geo_distance_m(lat, lon,
                                            profile->control_lat_e7[i] * 1e-7,
                                            profile->control_lon_e7[i] * 1e-7);
        if (!isfinite(d)) continue;

        if ((int)i == held) held_d = d;

        /* Nearest wins, among those actually in range. Nearest rather than
           strongest-claimed, because the radius is somebody's estimate of
           coverage and the distance is a fact. */
        if (d <= (double)radius && (best < 0 || d < best_d)) {
            best = (int)i;
            best_d = d;
        }
    }

    /* Nothing in range. Keep whatever is tuned - a site just out of reach
       still hears better than a site two counties away, and a route between
       coverage islands would otherwise spend the gap retuning. */
    if (best < 0) return -1;

    /* Already there. Refresh the age so the answer stays live, and report
       the distance for the panel, but do not ask for a retune. */
    if (best == held) {
        state->fix_us = fix_us;
        state->distance_m = (uint32_t)(best_d + 0.5);
        return -1;
    }

    /* Held one still comfortably in range: stay. The margin is what stops a
       boundary turning into a switch every few seconds, and every switch
       costs a re-acquisition of the control channel. */
    if (held >= 0 && isfinite(held_d)) {
        const uint32_t held_radius = profile->control_radius_m[held];
        if (held_d <= (double)held_radius + (double)P25_GEO_HYSTERESIS_M) {
            state->fix_us = fix_us;
            state->distance_m = (uint32_t)(held_d + 0.5);
            return -1;
        }
    }

    state->chosen = best;
    state->fix_us = fix_us;
    state->distance_m = (uint32_t)(best_d + 0.5);
    state->have_answer = true;
    return best;
}
