/* WARDRIVE's tracker: which devices are moving with the board (a car's
   hotspot two lanes over for 20 km, a tag in the bag), and which IDs belong
   to one vehicle (four TPMS sensors heard together, a car hotspot and its
   phone key). Sets WD_T_FOLLOW on a device once it has been heard in enough
   separate places over enough time.

   Called by wd_core under wd_lock; pure C, tested on the bench. The query
   calls below are for the HUNT page, also under wd_lock.

   How it decides. The board's odometer is cut into steps of WD_TRK_STEP_M;
   every device keeps which of the last 64 steps it was heard in, where the
   board was when it was first heard, how far the board has since been from
   there while still hearing it (the span), and how many places 300 m apart
   it was heard at. A coffee shop's access point is heard over a few hundred
   metres of road and its span stays under its range; something in the next
   lane is heard from kilometres apart. FOLLOW wants all of: the shared
   "with you" rule's minutes (ls_tail.h, 10 by default), 4 places, 4 steps,
   and a span past both the rule's metres and the kind's range (Wi-Fi
   1.5 km, Bluetooth and sub-GHz 900 m, LoRa 5 km; cells, aircraft and
   drones never).
   Parked, the steps stand still, so nothing is learned from sitting beside a
   house. Vehicles: followers, car radios and sub-GHz sensors whose steps,
   taken four at a time, match (3+ shared, 3/4 of their union) are one
   vehicle; its number stays while any of its members stays in it. */
#ifndef WD_TRACK_H
#define WD_TRACK_H

#include "wd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A new session (live or replay). */
void wd_track_reset(void);
/* Each sighting, after wd_core updated device `idx`. May set e->tags. */
void wd_track_note(int idx, wd_dev_t *e, bool added, uint32_t now);
/* Once per worker pass, under wd_lock. */
void wd_track_tick(uint32_t now);

/* ---- for the HUNT page (under wd_lock) ---- */

#define WD_TRK_STEP_M   150.0f  /* the odometer step */
#define WD_VEH_MAX      24      /* vehicles kept; a full table drops the one gone longest */
#define WD_VEH_MEMBERS  8
#define WD_VEH_TRAIL    32      /* steps of the convoy strip */

typedef struct {
    uint32_t first_ms, last_ms;
    float span_m;               /* farthest the board got from where it first heard it, still hearing it */
    float with_m;               /* the board's odometer from first to last sighting */
    uint8_t places;             /* places 300 m apart it was heard at */
    uint8_t steps;              /* steps it was heard in, of the last 64 */
    uint16_t veh;               /* its vehicle's number, 0 none */
    bool placed;                /* first_x/z hold where the board first heard it */
    float first_x, first_z;     /* session metres */
} wd_trk_t;

typedef struct {
    uint16_t num;               /* stable: VEHICLE 3 stays 3 */
    uint8_t n;                  /* members */
    uint8_t kind[WD_VEH_MEMBERS];
    uint8_t addr[WD_VEH_MEMBERS][6];
    int16_t idx[WD_VEH_MEMBERS];        /* wd_dev_at index, -1 left the table */
    uint16_t tags;              /* the members' tags together */
    uint32_t first_ms, last_ms;
    float with_m;               /* distance travelled together */
    int8_t trail[WD_VEH_TRAIL]; /* the strongest member in each of the last steps, oldest first; -128 unheard */
} wd_veh_t;

bool wd_track_info(int idx, wd_trk_t *out);
int wd_track_vehicles(void);
const wd_veh_t *wd_track_vehicle(int i);
/* The vehicle with number `num`, NULL none. */
const wd_veh_t *wd_track_vehicle_num(uint16_t num);
int wd_track_following(void);           /* devices with WD_T_FOLLOW now */
uint32_t wd_track_steps(void);          /* odometer steps this session: the convoy strip moves on these */

/* Alerts for the page to show, newest kept when the queue is full. */
enum { WD_AL_SIG, WD_AL_FOLLOW, WD_AL_VEHICLE };
typedef struct {
    uint8_t why, kind;
    uint8_t addr[6];
    uint16_t tags, sig, veh;
    int8_t rssi;
    int16_t idx;
    uint32_t ms;
    char name[24];
} wd_alert_t;
bool wd_track_alert(wd_alert_t *out);   /* the oldest pending one, taken */
uint32_t wd_track_hits(void);           /* alerts raised this session; the HUNT ring pulses when it changes */
uint32_t wd_track_hit_ms(void);         /* when the last one was raised */

#ifdef __cplusplus
}
#endif
#endif
