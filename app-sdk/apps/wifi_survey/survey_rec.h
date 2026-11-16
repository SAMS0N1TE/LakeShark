/* WIFI SURVEY's recorder: every Wi-Fi access point and Bluetooth device
   heard on a walk, with where the board was, as the WiGLE CSV that LINK's
   SURVEY tab wrote to /sdcard/lakeshark/survey (that recorder moved here).

   Pure C, no SDK calls: the app feeds observations and a GPS fix and writes
   the text this returns. The rules are LINK's:
   - one entry per address (Wi-Fi BSSID; BLE address and address type), the
     strongest sighting kept with the position it had, if any;
   - a position counts only from a fix at most 10 s old;
   - ONLY GPS FIX ignores every observation made without such a fix;
   - a row is written for the first sighting with a UTC time, again when the
     signal is 6 dB above the last row's, and again when the board has moved
     50 m from where the last row was positioned;
   - a full table gives up the device heard least recently, writing its
     summary row first, so a summary file holds every device's centroid;
   - the summary centroid weights every fresh-fix sighting by its power. */
#ifndef SURVEY_REC_H
#define SURVEY_REC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SR_CAP 4096                 /* devices in the table */
#define SR_BUCKETS 1024             /* address hash */
#define SR_FRESH_MS 10000           /* a fix older than this places nothing */
#define SR_SAMPLE_MS 1000           /* one sighting a second a device counts */
#define SR_UTC_MIN 1577836800u      /* 2020-01-01: older is a clock never set */
enum { SR_WIFI, SR_BLE };
enum { SR_IGNORED = -1, SR_UPDATED = 0, SR_ADDED = 1 };

typedef struct {
    bool fix;                       /* the receiver has a position */
    double lat, lon;
    float alt_m, hdop;              /* not finite or <= 0: unknown */
    int64_t fix_ms;                 /* when it was taken, on the caller's clock */
} sr_fix_t;

/* One sighting. Addresses are written most significant octet first. */
typedef struct {
    uint8_t kind, addr[6], addr_type, channel, sec;   /* sec: LAP_DOT11_SEC_* (Wi-Fi) */
    int rssi;
    char name[33];
} sr_obs_t;

typedef struct {
    uint8_t kind, addr[6], addr_type, channel, sec;
    int8_t rssi, row_rssi;          /* strongest heard; signal of the last row */
    bool positioned, logged, row_positioned;
    char name[33];
    uint32_t first_s;               /* UTC of the first sighting with a time, 0 none */
    uint32_t first_ms, last_ms, sample_ms;
    uint32_t seen, samples;         /* sightings counted; those with a fresh fix */
    double lat, lon;                /* where the strongest sighting was made */
    double row_lat, row_lon;
    /* Weighted centroid of every fresh-fix sighting, as offsets in degrees
       from the first of them (so a device seen across the date line works). */
    double weight, ref_lat, ref_lon, sum_lat, sum_lon;
    uint16_t next;                  /* hash chain: index + 1, 0 at the end */
} sr_entry_t;

typedef struct {
    sr_entry_t *e;
    uint16_t *head;                 /* SR_BUCKETS chains */
    unsigned count, cap, dropped, current;
    bool only_fix;
    unsigned totals[2], security[5], positioned;
    uint32_t minute_stamp[60];
    unsigned minute[60][2];
    /* A device leaving a full table: write its summary row now. */
    void (*evicted)(void *ctx, const sr_entry_t *e);
    void *ctx;
} sr_session_t;

void sr_init(sr_session_t *s, sr_entry_t *table, uint16_t *heads, unsigned cap, bool only_fix);
bool sr_fix_fresh(const sr_fix_t *g, int64_t now_ms);
/* SR_ADDED, SR_UPDATED (s->current is its entry), or SR_IGNORED: no fresh
   fix with ONLY GPS FIX on, or the device already counted this second. */
int sr_note(sr_session_t *s, const sr_obs_t *o, const sr_fix_t *g, uint32_t utc, int64_t now_ms);
bool sr_relog(const sr_entry_t *e, const sr_obs_t *o, const sr_fix_t *g, int64_t now_ms);
void sr_logged(sr_entry_t *e, const sr_obs_t *o, const sr_fix_t *g, int64_t now_ms);
bool sr_centroid(const sr_entry_t *e, double *lat, double *lon);
/* New devices of each kind in the last 60 s. */
void sr_minute(const sr_session_t *s, int64_t now_ms, unsigned out[2]);

/* Text, NUL-terminated; each returns its length, or -1 when it does not fit
   (or, for a row, without a UTC first sighting). */
int sr_header(char *out, size_t cap, const char *release, const char *board);
int sr_summary_header(char *out, size_t cap);
/* g positions the row when it is non-NULL and g->fix is set. */
int sr_row(char *out, size_t cap, const sr_obs_t *o, const sr_fix_t *g, uint32_t first_seen);
int sr_summary(char *out, size_t cap, const sr_entry_t *e);
/* YYYYMMDD_HHMMSS.csv */
bool sr_filename(char *out, size_t cap, uint32_t utc);
/* A file this recorder names (the only ones KEEP SESSIONS removes). */
bool sr_session_name(const char *name);
/* "YYYY-MM-DD HH:MM:SS" */
bool sr_utc_text(char out[20], uint32_t utc);
/* WiGLE AuthMode for a LAP_DOT11_SEC_* suite. */
const char *sr_auth(unsigned sec);
/* Open, WEP, WPA, WPA2, WPA3 as bits 0..4; mixed modes set both. */
unsigned sr_security(unsigned sec);
/* sort 0 strongest, 1 newest, 2 name; ties by kind, address, address type. */
int sr_compare(const sr_entry_t *a, const sr_entry_t *b, int sort);
#endif
