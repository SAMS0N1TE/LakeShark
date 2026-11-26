/* PARTY: a few LakeShark users together. Who is in the party and where,
   what each of them has found (Flock cameras, drones, police gear, trackers,
   the same car twice...) and roughly where it was, small files passed
   round, and a buzz in the pocket when something you asked for turns up.

   One party at a time, set up from a named profile (party_prof_t) and run
   by party_tick: the board's worker every 100 ms, the frame in the
   simulator. The screen reads the state under party_io_lock(). Radio
   traffic goes through party_link.h; the wire format is party_proto.h. */
#ifndef PARTY_CORE_H
#define PARTY_CORE_H

#include "../ls_alert.h"
#include <stdbool.h>
#include <stdint.h>

#include "party_link.h"
#include "party_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PARTY_PROFILES 6
#define PARTY_KW       3
#define PARTY_MEMBERS  12
#define PARTY_TRAIL    10
#define PARTY_PINGS    48
#define PARTY_RXFILES  8

/* How a class alerts. */
/* the shared alert modes (ls_alert.h): the saved values are unchanged, and
   BANNER+BUZZ is the one new */
enum { PARTY_STYLE_OFF = LS_ALERT_OFF, PARTY_STYLE_BANNER = LS_ALERT_BANNER, PARTY_STYLE_VIBE = LS_ALERT_BUZZ,
       PARTY_STYLE_SOUND = LS_ALERT_SOUND, PARTY_STYLE_BOTH = LS_ALERT_BOTH, PARTY_STYLES = LS_ALERT_N };
/* Vibration patterns (party_vibe_name). */
enum { PARTY_VIBES = 8 };
/* Position rate. SMART: 10 s in a vehicle or above 5 m/s, 30 s walking,
   2 min standing; and early after 50 m. */
enum { PARTY_RATE_SMART, PARTY_RATE_10S, PARTY_RATE_30S, PARTY_RATE_1M, PARTY_RATE_5M, PARTY_RATES };
enum { PARTY_AIR_1, PARTY_AIR_3, PARTY_AIR_10, PARTY_AIR_25, PARTY_AIRS };
enum { PARTY_NEAR_ANY, PARTY_NEAR_500M, PARTY_NEAR_2KM, PARTY_NEAR_10KM, PARTY_NEARS };
enum { PARTY_RSSI_ANY, PARTY_RSSI_85, PARTY_RSSI_75, PARTY_RSSI_65, PARTY_RSSIS };

typedef struct {
    char name[16];                 /* the profile's own name: "COMMUTE", "PROTEST" */
    char party[16];                /* what the party is called on screen */
    char code[24];                 /* the party code everyone types: the key */
    char call[PARTY_CALL_MAX + 1]; /* my call sign */
    uint8_t glyph;                 /* my mark on the radar: a printable character */
    uint8_t colour;                /* TUI_* | TUI_BRIGHT */
    uint8_t share, rate, vehicle;  /* PARTY_SHARE_*, PARTY_RATE_*, I am in a vehicle */
    uint16_t send_mask, recv_mask; /* PARTY_CMASK bits */
    uint8_t style[PARTY_CLASSES];  /* PARTY_STYLE_* per class */
    uint8_t vibe[PARTY_CLASSES];   /* pattern per class */
    char kw[PARTY_KW][13];         /* KEYWORD class: words in device names */
    uint8_t quiet, quiet_from, quiet_to;   /* quiet hours: sound becomes vibration */
    uint8_t mode;                  /* PARTY_MODE_* */
    uint8_t airtime;               /* PARTY_AIR_* */
    uint8_t files;                 /* take files: 0 off, 1 automatically */
    uint8_t near;                  /* PARTY_NEAR_*: alert only for finds this close */
    uint8_t min_rssi;              /* PARTY_RSSI_*: send only finds this strong */
    uint8_t pocket;                /* auto pocket lock: 0 off, 30 s, 1 min, 2 min */
    uint8_t north_up;              /* radar: 1 north up, 0 heading up when there is a compass */
} party_prof_t;

typedef struct {
    bool used;
    uint8_t id;
    char call[PARTY_CALL_MAX + 1];
    uint8_t glyph, colour, caps, share, batt;
    uint16_t want;
    bool pos;                      /* a position has been heard */
    double lat, lon;
    uint16_t acc_m;
    float speed, course, heading;  /* < 0 unknown */
    uint8_t pflags;                /* PARTY_POS_* */
    double tlat[PARTY_TRAIL], tlon[PARTY_TRAIL];   /* earlier positions, newest first */
    uint8_t ntrail;
    uint32_t last_ms, pos_ms, join_ms;
    int8_t rssi, snr;
    uint8_t via, seq;
    uint16_t heard, lost;
} party_member_t;

typedef struct {
    bool used;
    bool mine;                     /* found by this board */
    uint8_t from;                  /* member number of the first to report it */
    char who[PARTY_CALL_MAX + 1];
    uint16_t id;
    uint8_t cls, kind, flags;
    uint16_t tags;
    int8_t rssi;
    double lat, lon;
    uint16_t unc_m;
    uint32_t seen_ms;              /* when it was heard, this board's clock */
    uint32_t rx_ms;                /* when the newest report arrived */
    uint8_t addr[6];
    char label[PARTY_LABEL_MAX + 1];
    uint8_t nrep;                  /* how many members have reported it */
    uint8_t rep[4];
    float spread_m;                /* the farthest apart two reports were: a car on the move */
} party_pingrec_t;

enum { PARTY_X_NONE, PARTY_X_OFFER, PARTY_X_SEND, PARTY_X_WAIT, PARTY_X_DONE, PARTY_X_FAIL };

typedef struct {
    uint8_t state;                 /* PARTY_X_* */
    char name[PARTY_FNAME_MAX + 1];
    uint32_t size;
    uint16_t nchunks, sent, acked; /* acked: the fewest any receiver has */
    uint8_t receivers, done;
    uint32_t start_ms, end_ms;
    char why[24];
} party_xfer_view_t;

typedef struct {
    char name[PARTY_FNAME_MAX + 1];
    char from[PARTY_CALL_MAX + 1];
    uint32_t size, got_bytes;
    uint32_t ms;
    uint8_t state;                 /* PARTY_X_SEND while arriving, DONE, FAIL */
} party_rxfile_t;

/* A find from a detection source (WARDRIVE, or anything else that wants
   the party to know). */
typedef struct {
    uint8_t addr[6];
    uint8_t kind;                  /* WD_* kind */
    uint16_t tags;                 /* WD_T_* */
    int8_t rssi;
    char name[33];
    bool placed;                   /* lat/lon is where it was strongest */
    double lat, lon;
    uint32_t ms;                   /* when it was last heard */
} party_find_t;

typedef struct {
    bool running;
    bool link_up;
    char link_why[32];
    party_link_stats_t link;
    party_prof_t prof;
    party_key_t key;
    uint8_t me, seq;
    uint32_t start_ms;
    bool fix; double lat, lon; float acc_m, speed, course, heading;
    uint32_t pos_tx_ms, hello_tx_ms;
    double pos_tx_lat, pos_tx_lon;
    bool help;
    party_member_t member[PARTY_MEMBERS];
    party_pingrec_t ping[PARTY_PINGS];
    uint16_t ping_seq;
    uint32_t pings_in, pings_out, finds_seen;
    uint32_t air_used_ms;          /* estimate over the last minute */
    party_xfer_view_t tx;
    party_rxfile_t rxf[PARTY_RXFILES];
    uint8_t nrxf;
    /* what the motor is doing: pattern, step, when it started */
    int8_t vibe_pat;
    uint32_t vibe_ms;
    bool vibe_motor;               /* the board has one that answers */
    /* FIND: homing on one thing, buzzing faster as it gets closer */
    uint8_t find_kind;             /* 0 none, 1 member, 2 ping */
    uint16_t find_id;              /* member number, or ping slot */
    float find_m, find_brg;
    uint32_t find_buzz_ms;
    uint32_t heard_ms[16];         /* when the last datagrams arrived: the screen's fire */
    uint8_t heard_i;
    bool wd_hooked;                /* WARDRIVE's finds can be read */
    bool wd_running;
} party_t;

/* ---- life ---- */
bool party_init(void);                 /* allocates; false when out of memory */
const party_t *party_state(void);      /* read under party_io_lock() */
bool party_start(void);                /* with the current profile */
void party_stop(void);
bool party_running(void);
/* One service pass: how long until the next one wants to run (ms), 0 once
   the party has stopped. 20 while the motor plays a pattern, else 100. */
int  party_tick(void);

/* ---- profiles (UI task) ---- */
int  party_prof_count(void);
int  party_prof_cur(void);
void party_prof_select(int i);
party_prof_t *party_prof(int i);       /* the stored profile, edited in place */
int  party_prof_new(const char *name); /* index, -1 full */
void party_prof_delete(int i);
void party_prof_defaults(party_prof_t *p);
void party_prof_preset(party_prof_t *p, int preset);   /* WATCH FOR presets, see party_preset_name */
const char *party_preset_name(int preset);
int  party_preset_count(void);
bool party_prof_save(void);
void party_prof_load(void);
/* A setting of the running party changed: send it to the others now. */
void party_prof_applied(void);

/* ---- doing things ---- */
void party_mark(int cls, const char *label);   /* a ping of where I am */
void party_set_help(bool on);
bool party_send_file(const char *path, char *why, int cap);
void party_find_set(int kind, int id);         /* 0 stops */
void party_vibe_play(int pattern);
void party_offer_find(const party_find_t *f);  /* any detection source */

/* ---- for the screen and for other apps ---- */
const char *party_vibe_name(int pattern);
const char *party_style_name(int style);
/* A notice waiting for the UI task, true when there was one. */
bool party_notice_pop(char *title, int tcap, char *body, int bcap, uint8_t *hue, bool *quiet);
/* Ground distance (m) and true bearing (deg) from 1 to 2. */
void party_geo(double lat1, double lon1, double lat2, double lon2, float *metres, float *bearing);
/* How far a find probably was from where it was heard, from its RSSI. */
uint16_t party_unc_m(int kind, int rssi, float acc_m);
/* Members' pings newer than `since_ms` (party clock), for WARDRIVE to show
   as WD_T_PEER finds. Fills up to max, returns how many. */
int party_peer_since(party_find_t *out, int max, uint32_t since_ms);
int party_member_count(void);

#ifdef __cplusplus
}
#endif
#endif
