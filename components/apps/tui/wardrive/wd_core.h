/* WARDRIVE's session: every Wi-Fi network, Bluetooth device, cell and drone
   heard on a drive, where the board was when it heard it, and the drive
   itself.

   One session at a time, started and stopped by the user. The worker
   (wd_step: a task on the board, the frame in the simulator) reads the feed
   (wd_feed.h), notes what it heard here, appends WiGLE rows to the live log
   and writes the exports. The screen reads the same state under wd_lock().

   The CSV is WigleWifi-1.4: one row the first time a network is heard with a
   position and a clock, again when it is 6 dB stronger than its last row, and
   again 50 m on from where that row was. Wi-Fi and Bluetooth only: WiGLE has
   no row for an LTE cell known by EARFCN and PCI alone, nor for a drone; those
   go into the GPX as waypoints beside the track. */
#ifndef WD_CORE_H
#define WD_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What was heard. RF: a sub-GHz device by its own ID (TPMS sensor, weather
   station, remote); AIR: an aircraft by ICAO address; LORA: a LoRa node
   (Meshtastic, MeshCore, LoRaWAN) by node ID. New kinds go at the end: saved
   drives store the number. */
enum { WD_WIFI, WD_BLE, WD_CELL, WD_DRONE, WD_RF, WD_AIR, WD_LORA, WD_KINDS };
/* Which radio heard it. */
enum { WD_RADIO_C6, WD_RADIO_LORA, WD_RADIO_SDR, WD_RADIO_PEER, WD_RADIOS };
/* What the signature table (wd_sig.h) and the tracker (wd_track.h) say a
   device is; any number at once. */
enum {
    WD_T_CAMERA   = 1u << 0,    /* a camera: doorbell, CCTV, dashcam */
    WD_T_ALPR     = 1u << 1,    /* a plate reader (Flock and the like) */
    WD_T_BODYCAM  = 1u << 2,
    WD_T_POLICE   = 1u << 3,    /* police or fleet vehicle gear */
    WD_T_TRACKER  = 1u << 4,    /* AirTag, Tile, SmartTag... */
    WD_T_DRONE    = 1u << 5,
    WD_T_VEHICLE  = 1u << 6,    /* a car's own radio: hotspot, TPMS, phone key */
    WD_T_SKIMMER  = 1u << 7,
    WD_T_HACKTOOL = 1u << 8,    /* Flipper, Pineapple, deauther... */
    WD_T_WEARABLE = 1u << 9,    /* camera glasses and the like */
    WD_T_INFRA    = 1u << 10,   /* gunshot sensors, traffic, utility */
    WD_T_WATCH    = 1u << 11,   /* on the user's own watch list */
    WD_T_FOLLOW   = 1u << 12,   /* moving with the board (wd_track.h) */
    WD_T_REGULAR  = 1u << 13,   /* seen on earlier drives here (wd_atlas.h) */
    WD_T_NEWHERE  = 1u << 14,   /* never seen on this route before */
    WD_T_PEER     = 1u << 15,   /* reported by a party member or home base */
};
/* Wi-Fi security as the scan reports it. */
enum {
    WD_AUTH_OPEN, WD_AUTH_WEP, WD_AUTH_WPA, WD_AUTH_WPA_WPA2, WD_AUTH_WPA2, WD_AUTH_WPA2_EAP,
    WD_AUTH_WPA3, WD_AUTH_WPA2_WPA3, WD_AUTH_OWE, WD_AUTH_WAPI, WD_AUTH_WPA3_EAP, WD_AUTH_UNKNOWN
};
/* ...and the five classes the counters show. */
enum { WD_SEC_OPEN, WD_SEC_WEP, WD_SEC_WPA, WD_SEC_WPA2, WD_SEC_WPA3, WD_SECS };

#define WD_CAP      4096        /* devices held; the least recently heard leaves a full table */
#define WD_TRAIL    8192        /* track points; a full track keeps every other one */
#define WD_DROPS    48          /* the newest finds, for LIVE's rain */
#define WD_ROWS_BUF 16384       /* CSV text between worker passes */

typedef struct {
    bool fix;                   /* a position from the last few seconds */
    double lat, lon;
    float alt_m, hdop;          /* not finite or <= 0: unknown */
    float speed_mps;            /* < 0: unknown */
    float course_deg;           /* < 0: unknown */
    float compass_deg;          /* < 0: no compass */
    uint8_t sats;
    uint32_t utc;               /* Unix seconds now; 0 while there is no clock */
} wd_fix_t;

/* One sighting from the feed. Addresses most significant octet first. */
typedef struct {
    uint8_t kind, auth, channel;
    uint8_t addr[6];
    int8_t rssi;
    bool own_pos;               /* a drone that reports where it is */
    double lat, lon;
    char name[33];
    uint8_t radio;              /* WD_RADIO_* (0: the C6, as before) */
    uint8_t sub;                /* the kind's own subtype: TPMS model, mesh flavour... */
    uint16_t tags;              /* WD_T_* the source already knows; the table adds more */
    uint32_t freq_khz;          /* 0: not known or not meaningful */
    /* Bluetooth advert IDs for the signature table: ad_have bit 0 company +
       mfg0, bit 1 uuid16 + svc0 (a flag: feeds zero the struct). */
    uint8_t ad_have;
    uint16_t company, uuid16;
    int16_t mfg0, svc0;
} wd_obs_t;

#define WD_F_PLACED 1           /* lat/lon hold where it was strongest */
#define WD_F_ROW    2           /* a CSV row has been written */
typedef struct {
    uint8_t kind, auth, channel, flags;
    uint8_t addr[6];
    int8_t rssi, last_rssi, row_rssi;
    char name[33];
    uint32_t first_ms, last_ms, sample_ms;
    uint32_t seen;
    double lat, lon, row_lat, row_lon;
    float x, z, alt;            /* session metres: east, south */
    uint16_t next;              /* hash chain, index + 1 */
    uint16_t tags;              /* WD_T_* */
    uint16_t sig;               /* the matching signature + 1 (wd_sig.h), 0 none */
    uint8_t radio, sub;
    uint32_t freq_khz;
    uint8_t posted;             /* what went to ls_seen: 1 the find, 2 that it follows */
} wd_dev_t;

typedef struct { double lat, lon; float x, z, alt, speed; uint32_t ms; } wd_pt_t;

typedef struct {
    uint8_t kind, sec;
    int8_t rssi;
    uint8_t lane;
    uint32_t ms;
    char name[24];
    uint16_t tags;
} wd_drop_t;

typedef struct {
    bool running, ever, origin, only_fix;
    bool replay;                /* the session is a saved drive played back (wd_replay.h) */
    bool src[WD_KINDS];         /* which sources the session listens to */
    uint32_t start_ms, stop_ms;
    double lat0, lon0;
    float m_lon, m_lat;         /* metres in a degree at the origin */
    wd_fix_t fix;
    uint32_t fix_ms;            /* when the last good fix arrived, 0 none */
    bool fix_weak;              /* the GPS has a position but not one to trust (HDOP, satellites, settling) */
    uint8_t settle;             /* good fixes in a row that agree, since start or a loss */
    unsigned fixes_dropped;     /* fixes the gate kept out of the track (weak, settling, impossible jumps) */
    bool wifi_fix;              /* the position now is from known access points (wd_atlas_locate), not the GPS */
    uint8_t wifi_used;          /* how many it used */
    float wifi_err_m;
    uint32_t wifi_ms;
    float wifi_m;               /* distance driven on Wi-Fi fixes this drive */
    float x, z;                 /* where the board is, session metres */
    float speed, heading;       /* m/s; degrees true, < 0 unknown */
    float dist_m;
    unsigned total[WD_KINDS], sec[WD_SECS], placed, evicted, rows, rows_lost;
    unsigned scans;             /* completed Wi-Fi scans: LIVE's arcs and 3D's pulse step on these */
    uint32_t scan_ms;
    uint32_t sec_stamp[60];
    uint16_t sec_new[60];       /* new devices in each of the last 60 seconds */
    uint16_t min_new[30];       /* ...and in each of the last 30 minutes */
    uint32_t min_stamp[30];
    long utc_base;              /* utc - ms/1000, 0 until a clock was seen */
    char stamp[24];             /* the session's file name stem */
    char csv[128], gpx[128];    /* the files written, "" none */
    char note[72];              /* the last thing the worker has to say */
    int card;                   /* 1 card ok, 0 no card, -1 write failed */
} wd_stats_t;

/* ---- the session (callers hold wd_lock) ---- */

bool wd_begin(uint32_t now_ms, const bool src[WD_KINDS], bool only_fix);
void wd_end(uint32_t now_ms);
/* A session for REPLAY: every source, nothing written, nothing read from
   the scanners; the replay feeds wd_fix_note and wd_note itself. */
bool wd_begin_replay(uint32_t now_ms);
void wd_set_sources(const bool src[WD_KINDS], bool only_fix);
const wd_stats_t *wd_stats(void);
int  wd_dev_count(void);
const wd_dev_t *wd_dev_at(int i);
int  wd_trail_count(void);
const wd_pt_t *wd_trail_at(int i);
/* Newest first, i from 0; NULL past the end. */
const wd_drop_t *wd_drop_at(int i);
/* New devices in the last minute, and in minute `ago` (0 this one). */
unsigned wd_new_last_minute(uint32_t now_ms);
unsigned wd_new_in_minute(uint32_t now_ms, int ago);
/* The class of a Wi-Fi security, and its WiGLE capability string. */
int  wd_sec_class(unsigned auth);
const char *wd_auth_name(unsigned auth);
const char *wd_auth_short(unsigned auth);
/* Session metres of a place. */
void wd_local(double lat, double lon, float *x, float *z);
/* A kind's name ("WI-FI", "BLUETOOTH", "CELL", "DRONES", "SUB-GHZ",
   "AIRCRAFT", "LORA"), its short word ("WI-FI", "BT", "CELL", "DRONE", "RF",
   "AIR", "LORA") and its three-letter tag for lists ("BT", "LTE", "UAS",
   "RF", "AIR", "LRA"; Wi-Fi lists show the security instead). */
const char *wd_kind_name(int kind);
const char *wd_kind_short(int kind);
const char *wd_kind_tag(int kind);
/* The highest-priority WD_T_* bit's word ("ALPR", "BODYCAM", ...), "" none. */
const char *wd_tag_word(uint16_t tags);

/* ---- the drives on the card ----
   Each drive in its own folder: <dir>/<stem>/<stem>.csv, <stem>.gpx and
   drive.txt (its summary). Older drives were loose files in <dir>; the
   readers still find them, and wd_drives_tidy moves them in. */
#define WD_STEM 28
typedef struct {
    uint32_t start_utc, secs;   /* 0: not known */
    float km;
    unsigned finds, targets;    /* 0 for a drive summarised from its track */
    long bytes;
    bool track, log, summary;
    double lat, lon;            /* where it began, 0 not known */
} wd_drive_info_t;
/* A drive's file (ext "csv", "gpx", "txt"): the old loose one while it is
   there, else in the drive's folder; make creates the folder (to write). */
bool wd_drive_path(char *out, size_t cap, const char *stem, const char *ext, bool make);
/* The drives on the card, newest first. */
int  wd_drives(char (*stem)[WD_STEM], int max);
/* A drive's summary as far as it is known without reading its files:
   drive.txt, sizes, the date in its stem (summary false: no drive.txt yet). */
bool wd_drive_info(const char *stem, wd_drive_info_t *out);
/* Build an old drive's drive.txt from its log and track, reading at most
   budget bytes a call (the screen calls it a slice a frame). 1 done (out
   filled), 0 more to read. One drive at a time. A NULL stem drops the
   one half done and closes its file. */
int  wd_drive_summarize(const char *stem, size_t budget, wd_drive_info_t *out);
/* Move loose old drives into their folders; returns how many files moved. */
int  wd_drives_tidy(void);

/* ---- what the worker and the screen ask each other ---- */

enum { WD_REQ_EXPORT = 1, WD_REQ_LOCKER = 2 };
void wd_request(int req);                   /* under wd_lock */

/* One worker pass: read the feed, note, write. Takes the lock itself. */
void wd_step(uint32_t now_ms);
/* Something is left for the worker: a session, a request, an open log. */
bool wd_busy(void);

/* The pure parts, for a test. */
int  wd_note(const wd_obs_t *o, uint32_t now_ms);   /* 1 new, 0 known, -1 ignored */
void wd_fix_note(const wd_fix_t *f, uint32_t now_ms);
size_t wd_take_rows(char *out, size_t cap);
int  wd_csv_header(char *out, size_t cap, const char *board);
bool wd_utc_text(char out[20], uint32_t t);

#ifdef __cplusplus
}
#endif
#endif
