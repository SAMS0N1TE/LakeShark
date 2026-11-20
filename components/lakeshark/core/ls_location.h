/* ls_location: where the board is, for every app.

   The GPS runs from boot when Settings > GPS is on (the default) and the board
   has a receiver; the dark-screen power park still puts it in standby. Apps
   read the position here instead of starting and polling ls_gps themselves:

       ls_location_t l;  ls_location_get(&l);     live fix, quality, region
       double lat, lon;
       if (ls_location_here(&lat, &lon)) ...      best known: live fix, else the
                                                  last good one (kept across
                                                  reboots), else the saved HOME
       char key[24];  ls_location_key(key, sizeof(key));
                                                  "GPS 3D 9 sats" / "NO FIX" /
                                                  "GPS OFF" / "NO GPS" for a key;
                                                  ls_location_key_tap() on a tap

   All of it is RAM reads, safe from any task and from a UI frame. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_LOC_NO_GPS = 0,  /* the board has no receiver */
    LS_LOC_OFF,         /* Settings > GPS is off and nothing else runs it */
    LS_LOC_NO_FIX,      /* running, no position yet */
    LS_LOC_FIX,
} ls_loc_state_t;

typedef enum { LS_LOC_SRC_NONE = 0, LS_LOC_SRC_LIVE, LS_LOC_SRC_LAST, LS_LOC_SRC_HOME } ls_loc_src_t;

typedef struct {
    ls_loc_state_t state;
    bool     fix;           /* a position from the last few seconds */
    bool     have;          /* lat/lon hold a good fix: live, or the last one (also from before a reboot) */
    double   lat, lon;      /* degrees, north and east positive */
    float    alt_m, hdop;
    uint8_t  quality;       /* GGA: 0 none, 1 GPS, 2 DGPS */
    uint8_t  sats;          /* in the solution */
    uint8_t  dim;           /* 0 none, 2 or 3 */
    int64_t  fix_us;        /* esp_timer time of that fix; 0 when it is from before this boot */
    char     region[8];     /* "US-FL", "CA-ON", "AU-NSW", "DE", or "" (see ls_location_region) */
} ls_location_t;

/* Once at boot, after settings_init: loads the last fix, starts the receiver
   when the setting is on, starts the 2 s service tick. */
void ls_location_init(void);

void ls_location_get(ls_location_t *out);
/* Best known position; LS_LOC_SRC_NONE (lat/lon untouched) when there is none. */
ls_loc_src_t ls_location_here(double *lat, double *lon);

/* Settings > GPS. Setting it starts or stops the receiver and is remembered. */
bool ls_location_enabled(void);
void ls_location_set_enabled(bool on);
/* A consumer wants a position now: starts the receiver if the setting is on
   and it is not running. Cheap when it already is. */
void ls_location_want(void);

/* The text for a GPS state key, and the state for its colour. */
ls_loc_state_t ls_location_key(char *out, size_t cap);
/* A tap on that key: OFF turns the GPS on; otherwise nothing. */
void ls_location_key_tap(void);

/* Called on the location task (PSRAM stack: no direct flash or NVS writes,
   keep it short) with each first fix and whenever a fix is km or more from
   the one this subscriber was last called with. Up to LS_LOC_SUBS. */
typedef void (*ls_location_moved_fn)(double lat, double lon, void *ctx);
bool ls_location_on_moved(float km, ls_location_moved_fn fn, void *ctx);
#define LS_LOC_SUBS 4

/* Coarse region of a point: country, plus state or province for the US,
   Canada and Australia ("US-FL"). From a table of bounding boxes where the
   smallest box holding the point wins, so near a border it can name the
   neighbour. False and "" when no box holds it. */
bool ls_location_region(double lat, double lon, char out[8]);
/* Every US state whose box, grown by margin_deg, holds the point: USPS codes
   into out[][3]. Returns how many (at most cap). */
int ls_location_us_states(double lat, double lon, float margin_deg, char out[][3], int cap);

/* Ground distance in km (equirectangular; good to well under 1 % at pack ranges). */
float ls_location_km(double lat1, double lon1, double lat2, double lon2);

#ifdef __cplusplus
}
#endif
