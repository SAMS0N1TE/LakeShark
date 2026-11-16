/* WIFI WATCH: an 802.11 intrusion watch over raw capture. The engine
   (watch_core.c) reads frames and raises alerts; main.c draws and logs.
   Receive only: nothing is sent, and handshakes are noted, never kept. */
#ifndef WATCH_H
#define WATCH_H
#include "lakeshark_app.h"
#include "lap_dot11.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define W_APS 64
#define W_CLIENTS 64
#define W_PAIRS 32
#define W_SHAKES 24
#define W_ALERTS 128

enum { SEV_INFO, SEV_WARN, SEV_ALERT };
enum { K_DEAUTH, K_DISASSOC, K_FLOOD, K_TWIN, K_HIDDEN, K_HANDSHAKE, K_PMKID, K_COUNT };
extern const char *const watch_kind_name[K_COUNT];

typedef struct {
    uint32_t ms;                /* lap_millis when raised */
    int64_t unix;               /* wall clock, 0 unknown */
    uint8_t sev, kind, channel;
    char who[18], whom[18];     /* source and target addresses, "" none */
    char text[100];
} watch_alert_t;

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t sec, channel;
    bool hidden, revealed, twin_said, changed_said;
    int8_t rssi;
    uint32_t beacons, first_ms, last_ms;
} watch_ap_t;

typedef struct {
    uint8_t mac[6];
    bool random;
    int8_t rssi;
    uint32_t probes, last_ms;
    uint8_t nssid;
    char ssid[4][33];           /* the first four names it asked for; "" is a wildcard probe */
    bool wildcard;
} watch_client_t;

typedef struct {
    uint8_t src[6], dst[6];
    bool disassoc;
    uint16_t reason;
    uint8_t channel;
    uint32_t total, window, window_ms, said_ms, last_ms;
    uint16_t rate;              /* frames a second over the last window */
} watch_pair_t;

typedef struct {
    uint8_t ap[6], sta[6];
    uint8_t msgs;               /* bit n-1: message n of the 4-way handshake seen */
    bool pmkid, said, pmkid_said;
    uint32_t last_ms;
} watch_shake_t;

typedef struct {
    /* thresholds */
    uint16_t burst_frames;      /* deauth or disassoc frames from one pair ... */
    uint16_t burst_ms;          /* ... within this window raise an alert */
    uint16_t flood_new;         /* new BSSIDs within flood_ms raise a flood alert */
    uint16_t flood_ms;
} watch_cfg_t;

typedef struct {
    watch_cfg_t cfg;
    watch_ap_t ap[W_APS];
    watch_client_t cl[W_CLIENTS];
    watch_pair_t pair[W_PAIRS];
    watch_shake_t shake[W_SHAKES];
    int naps, ncl, npairs, nshakes;
    watch_alert_t alert[W_ALERTS];
    uint32_t nalerts;           /* ever raised; the ring keeps the newest W_ALERTS */
    uint32_t by_kind[K_COUNT];
    /* counters */
    uint32_t frames, mgmt, ctrl, data, beacons, probes, proberesp, deauth, disassoc, auth, assoc, eapol, protected_mgmt;
    uint32_t short_frames;
    uint32_t aps_over, clients_over;
    /* beacon flood: new BSSIDs in one-second buckets */
    uint16_t new_bss[8];
    uint32_t new_sec, flood_said_ms;
    int64_t unix_now;           /* stamped onto alerts */
} watch_t;

void watch_init(watch_t *w);
/* One frame as lap_capture_read gave it. Returns how many alerts it raised. */
int watch_frame(watch_t *w, const uint8_t *frame, size_t len, int8_t rssi, uint8_t channel, uint32_t now_ms);
const watch_alert_t *watch_alert(const watch_t *w, uint32_t index);   /* 0 is the newest; NULL past the end */
uint32_t watch_alerts_kept(const watch_t *w);

/* ---- alert delivery: what the owner chose, kept in the app's own file ---- */

enum { OUT_NONE, OUT_SOUND, OUT_VIBRATE, OUT_BOTH, OUT_COUNT };          /* per severity */
enum { MESH_OFF, MESH_CH0, MESH_CH1, MESH_CH2, MESH_CH3, MESH_DM, MESH_COUNT };   /* where a notice goes */
enum { BANNER_OFF, BANNER_QUIET, BANNER_BOARD, BANNER_COUNT };          /* lap_notify, lap_alert */
#define WATCH_PREFS_MAGIC 0x31505757u    /* "WWP1" */
#define WATCH_GAP_MIN_MAX 120            /* minutes between notices of one kind */
#define WATCH_PER_HOUR_MAX 30            /* notices an hour, all kinds */
typedef struct {
    uint32_t magic;
    uint16_t size, version;
    uint8_t mesh_to;            /* MESH_*; OFF until the owner turns it on */
    uint8_t mesh_min_sev;       /* SEV_*: quieter alerts stay on the board */
    uint16_t mesh_gap_min;      /* one notice per kind per this many minutes ... */
    uint8_t mesh_per_hour;      /* ... and no more than this many an hour in all */
    uint8_t out[3];             /* OUT_* for SEV_INFO, SEV_WARN, SEV_ALERT */
    uint8_t speak;              /* lap_say the alert too */
    uint8_t banner;             /* BANNER_* */
    uint8_t quiet_on, quiet_from, quiet_until;   /* local hours; from == until is all day */
    uint8_t quiet_mesh;         /* notices still go out over the mesh in quiet hours */
    char dm_id[17], dm_name[20];   /* the node MESH_DM writes to */
} watch_prefs_t;
void watch_prefs_default(watch_prefs_t *p);
/* A copy read back from the file: false (and the defaults) when it is not
   ours or is from another layout; out-of-range fields are brought in range. */
bool watch_prefs_load(watch_prefs_t *p, const void *data, size_t len);
/* Quiet hours at this local hour: from 22 until 7 covers 22:00-06:59. */
bool watch_quiet(const watch_prefs_t *p, int hour);

/* The mesh relay: at most one notice per alert kind per gap and at most
   per_hour notices an hour in all, so a deauthentication flood cannot flood
   the mesh. Alerts that arrive while their kind waits are counted and go
   out as one notice ("x12") once it may send again. Pure bookkeeping: the
   caller sends, and calls watch_relay_sent only when the mesh took it. */
#define W_RELAY_RING 32
typedef struct {
    uint32_t last_ms;           /* when this kind last went out */
    bool ever;                  /* it has gone out since the relay began */
    uint16_t pending;           /* alerts waiting, newest kept in `latest` */
    uint32_t first_ms;          /* when the oldest waiting one came */
    watch_alert_t latest;
} watch_relay_kind_t;
typedef struct {
    uint8_t min_sev;
    uint16_t gap_min;
    uint8_t per_hour;
    watch_relay_kind_t k[K_COUNT];
    uint32_t sent_ms[W_RELAY_RING];    /* the last sends, for the hourly cap */
    uint32_t nsent;                    /* ever sent */
    uint32_t noted, coalesced;         /* alerts taken; of them, folded into another notice */
} watch_relay_t;
void watch_relay_init(watch_relay_t *r, uint8_t min_sev, uint16_t gap_min, uint8_t per_hour);
bool watch_relay_note(watch_relay_t *r, const watch_alert_t *a);   /* false: below min_sev */
/* The kind to send now (the one waiting longest), or -1: nothing waits, or
   every waiting kind is inside its gap, or the hour's notices are spent. */
int watch_relay_next(const watch_relay_t *r, uint32_t now);
void watch_relay_sent(watch_relay_t *r, int kind, uint32_t now);
uint32_t watch_relay_waiting(const watch_relay_t *r);              /* alerts held back */
/* Milliseconds until something held may go (0 now or nothing held). */
uint32_t watch_relay_wait_ms(const watch_relay_t *r, uint32_t now);
/* The notice: "WIFI WATCH DEAUTH x12 02:aa:00:00:00:01>bcast ch6 14:03".
   kind_name NULL uses the alert's kind; count 1 shows no "x". */
int watch_mesh_text(char *out, size_t cap, const watch_alert_t *a, const char *kind_name, unsigned count, const char *hhmm);
#endif
