/* Which control channel the receiver should be on, given where it is.

   A P25 profile lists up to sixteen control channels. Today the operator
   picks one by hand, or presses CONTROL SURVEY and waits up to twenty-eight
   seconds while the radio dwells on each in turn scoring live traffic. On a
   drive that is the wrong instrument: the answer is already known from the
   map, and the sites you are leaving behind do not need to be measured to be
   ruled out.

   This is the decision, on its own, with no radio and no clock of its own -
   which is what lets the bench prove it. The caller supplies the position
   and the time; this returns an index or "stay where you are".

   IT DOES NOT TUNE. p25_program_select_control does that, and it is
   deliberately the only thing that does. */

#ifndef LS_P25_GEO_H
#define LS_P25_GEO_H

#include <stdbool.h>
#include <stdint.h>

#include "p25_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A fix older than this is not used to make a new decision. Five seconds is
   the same freshness the conventional scanner's geometry demands, and for
   the same reason: a position from a minute ago is a position a mile back. */
#define P25_GEO_FIX_MAX_AGE_US   5000000

/* ...but an ANSWER already reached stays usable for this long after the fix
   stops arriving, so driving under a bridge does not throw away the site
   selection and start hunting. */
#define P25_GEO_HOLD_US          30000000

/* How much further than its radius a site is allowed to be before the
   receiver gives up on one it has already chosen. Without this, sitting on
   a boundary makes the radio oscillate between two sites for as long as you
   idle there, and every switch costs a re-acquisition. */
#define P25_GEO_HYSTERESIS_M     2000u

/* The smallest sensible coverage claim. A profile asking for less is almost
   certainly a units mistake - metres typed as kilometres - and a site that
   can never be entered is worse than one that is always eligible. */
#define P25_GEO_MIN_RADIUS_M     500u

typedef struct {
    /* The index this last settled on, or -1 for "nothing chosen yet". */
    int      chosen;
    /* When the fix behind `chosen` was taken. */
    int64_t  fix_us;
    bool     have_answer;
    /* Straight-line metres to the chosen site at that moment, for the panel
       and for the journal. Meaningless when have_answer is false. */
    uint32_t distance_m;
} p25_geo_t;

/* Great-circle metres between two points in degrees. Exposed because the
   bench checks it against known pairs - a direction-finding bug and a site
   selection bug look identical from the outside otherwise. */
double p25_geo_distance_m(double lat_a, double lon_a,
                          double lat_b, double lon_b);

/* Whether the profile carries enough position data to be followed at all.
   A profile with no coordinates is not broken - it is a single-site profile,
   and this simply has nothing to do with it. */
bool p25_geo_profile_has_sites(const p25_profile_t *profile);

/* THE DECISION.

   Returns the control index the receiver should be on, or -1 to change
   nothing. Returning -1 is the normal answer most of the time: the position
   has not moved enough to matter, or there is no fix, or the profile carries
   no coordinates.

   `now_us` and `fix_us` are the caller's clock. `valid` is the receiver's own
   view of whether the fix is usable; a false here is respected even if the
   numbers look plausible, because the GPS knows things this does not. */
int p25_geo_select(p25_geo_t *state, const p25_profile_t *profile,
                   bool valid, double lat, double lon,
                   int64_t fix_us, int64_t now_us);

/* True while the last answer is still worth acting on. */
bool p25_geo_ready(const p25_geo_t *state, int64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* LS_P25_GEO_H */
