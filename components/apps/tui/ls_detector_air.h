#ifndef LS_DETECTOR_AIR_H
#define LS_DETECTOR_AIR_H
/* DETECTOR's view of the aircraft the ADS-B/UAT decoder already tracks: read
 * from the shared table, matched by rules, never a radio of its own. */
#include "ls_detector.h"
#ifdef __cplusplus
extern "C" {
#endif
/* An aircraft that has not updated for this long never alerts. */
#define LS_DETECTOR_AIR_STALE_US (60 * 1000000LL)
/* Aircraft slots examined per pass; at least the decoder's table. */
#define LS_DETECTOR_AIR_SLOTS 16

typedef struct {
    uint32_t icao;
    uint8_t qualifier, emitter_tc, emitter_ca, emergency; /* emergency 0 = none or unknown */
    uint16_t squawk;          /* 0 when unknown or older than the stale limit */
    int32_t altitude_ft;      /* barometric as reported; INT32_MIN when unknown or stale */
    uint16_t distance_x10_km; /* LS_DETECTOR_AIR_NO_DISTANCE without a fresh fix on both ends */
    uint8_t sources;          /* adsb_aircraft_t.sources: 1 = 1090, 2 = UAT */
    char callsign[9];
    int64_t last_seen_us;
} ls_detector_air_t;

typedef struct { bool fix; double lat, lon; } ls_detector_air_gps_t;

/* A table row for the screen: the aircraft and the enabled rules it matches now. */
typedef struct { ls_detector_air_t a; uint64_t rules; } ls_detector_air_row_t;

/* Short name of the receiver another app is running, or NULL when none is. */
const char *ls_detector_air_source(void);
/* The receiver's position if it has a fix newer than three seconds. */
void ls_detector_air_gps(ls_detector_air_gps_t *out, int64_t now);
/* Table slot to a bounded snapshot. False for an empty or stale slot. */
bool ls_detector_air_read(int slot, int64_t now, const ls_detector_air_gps_t *gps, ls_detector_air_t *out);
/* All the rule's aircraft predicates hold; unknown fields never match. */
bool ls_detector_air_match(const ls_detector_rule_t *rule, const ls_detector_air_t *air);

/* ADS-B emitter category to a bit (0..15) or -1, and the rule-file name. */
int ls_detector_air_category_bit(unsigned tc, unsigned ca);
const char *ls_detector_air_category_name(unsigned bit);
/* A general-emergency squawk (7500, 7600, 7700) or an emergency state. */
bool ls_detector_air_alarm(unsigned squawk, unsigned emergency);
/* "UAL123 sq7700 35000ft 8.4km": the identifying facts of an aircraft or event. */
void ls_detector_air_text(const ls_detector_air_t *air, char *out, size_t cap);
void ls_detector_air_event_text(const ls_detector_event_t *event, char *out, size_t cap);
/* The aircraft table's rows for the screen; false if the worker holds the lock. */
bool ls_detector_air_view(ls_detector_air_row_t *rows, size_t cap, size_t *n);
#ifdef __cplusplus
}
#endif
#endif
