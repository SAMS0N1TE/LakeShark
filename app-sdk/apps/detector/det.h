/* DETECTOR's engine: rules, matching, cooldowns, quiet hours, the shared
 * alert line. No board calls: the app feeds it what the board's watchers saw
 * and hands its events to the sinks through det_t.deliver, and the bench
 * builds it as it is (bench/tests/test_detector_app.c).
 *
 * Rules are /sdcard/detector/rules.json, version 1, the format the built-in
 * DETECTOR read: data/detector/rules.json documents it. */
#ifndef DET_H
#define DET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DET_RULES 64
#define DET_HISTORY 24
#define DET_RULES_BYTES 32768
#define DET_COOLDOWNS 512
#define DET_AIR_SLOTS 16

/* Sources. The first ten are the board's LAP_SEEN_* bits (aircraft are read
   from lap_aircraft, the mesh from lap_mesh_*). */
enum {
    DET_RID = 1, DET_BLE = 2, DET_WIFI = 4, DET_LORA = 8, DET_ADSB = 16, DET_DRONE_WATCH = 32,
    DET_REC = 64, DET_CATCH = 128, DET_POCSAG = 256, DET_P25 = 512, DET_MESH_PEER = 1024, DET_MESH_ALERT = 2048,
};
#define DET_SIGNALS (DET_REC | DET_CATCH | DET_POCSAG | DET_P25 | DET_MESH_PEER | DET_MESH_ALERT)
#define DET_SOURCES (31 | DET_DRONE_WATCH | DET_SIGNALS)
/* Sinks. "flipper" still parses; the head has no DETECTOR to show it now. */
enum { DET_BANNER = 1, DET_VIBRATE = 2, DET_SD = 4, DET_TTS = 8, DET_TONE = 16, DET_FLIPPER = 32, DET_MESH = 64 };
/* SWEEP's categories and list sections (LAP_CAT_*, LAP_SECT_*). */
enum { DET_CAT_CAMERA = 0, DET_CAT_BODYCAM, DET_CAT_DRONE, DET_CAT_TRACKER, DET_CAT_ATTACK, DET_CATS };
/* Aircraft predicates (det_rule_t.air_mask). */
enum { DET_AIR_SQUAWK = 1, DET_AIR_CALL = 2, DET_AIR_ICAO = 4, DET_AIR_CAT = 8, DET_AIR_KM = 16, DET_AIR_ALT = 32, DET_AIR_EMERG = 64 };
#define DET_AIR_LIST 8
#define DET_NO_DISTANCE 0xFFFFu
/* Decoder and mesh predicates (det_rule_t.sig_mask). */
enum { DET_SIG_NUM = 1, DET_SIG_WORDS = 2, DET_SIG_FREQ = 4, DET_SIG_NOVEL = 8, DET_SIG_EMERG = 16, DET_SIG_EVENT = 32 };

typedef struct {
    char id[32];
    uint32_t cooldown_s, rule_cooldown_s;
    uint16_t sources;
    uint8_t categories, sinks, sections, bands;
    int8_t min_rssi_dbm;
    uint32_t min_hits_per_min;
    bool has_min_rssi;
    bool enabled, validated, has_validated;
    uint8_t air_mask, air_squawks, air_calls, air_icaos;
    uint16_t air_cats, air_km;
    int32_t air_alt_below_ft;
    uint16_t air_squawk[DET_AIR_LIST];
    char air_call[DET_AIR_LIST][9];
    uint32_t air_icao_lo[DET_AIR_LIST], air_icao_hi[DET_AIR_LIST];
    /* num_key: 1 capcode, 2 talkgroup, 3 unit. Words are upper case. */
    uint8_t sig_mask, num_key, nums, words, events;
    uint32_t num[8], khz_lo, khz_hi;
    char word[4][16];
} det_rule_t;

/* A sighting from the board: SWEEP, Remote ID, the drone link, drone watch. */
typedef struct {
    uint64_t entity;
    uint16_t source;
    uint8_t category, section;  /* 255 none */
    int8_t rssi;
    bool validated, flood, approaching;
    uint16_t band, rate_guess;
    uint32_t hits_per_min;
} det_obs_t;
/* A decoder or the mesh: REC, CATCH, POCSAG, P25, mesh peers and alerts. */
typedef struct {
    uint16_t source;
    uint8_t kind;       /* mesh.peer 0 new, 1 lost; mesh.alert the sender's source bit index */
    uint8_t flag;       /* REC/CATCH 1 new pattern; P25 1 emergency; mesh.alert 1 emergency, 2 test */
    int8_t rssi;
    uint32_t a, b;      /* REC/CATCH Hz,pattern; POCSAG capcode,function; P25 talkgroup,unit; mesh.alert seq */
    char who[17], name[17], text[64];
    uint16_t distance_x10_km, bearing;   /* mesh.alert, when both ends are known; else DET_NO_DISTANCE */
} det_signal_t;
/* An aircraft from lap_aircraft. */
typedef struct {
    uint32_t icao;
    uint8_t emergency;
    int8_t category;          /* det_air_category_bit, -1 unknown */
    uint16_t squawk;          /* 0 unknown */
    int32_t altitude_ft;      /* INT32_MIN unknown */
    uint16_t distance_x10_km; /* DET_NO_DISTANCE unknown */
    char callsign[9];
} det_air_t;

typedef struct {
    uint64_t entity, rules;
    int64_t at;
    uint32_t sequence, revision, coalesced, suppressed;
    uint16_t source;
    uint8_t category, selected, allowed, delivered, failed;
    int8_t rssi;
    uint16_t band, rate_guess;
    uint32_t hits_per_min;
    bool test, validated, relative_rssi, quiet;
    uint8_t builtin;          /* 1 drone-link-nearby, 2 mesh-alert */
    /* aircraft */
    uint32_t icao;
    uint8_t emergency;
    int8_t air_category;
    uint16_t squawk, distance_x10_km, bearing;
    int32_t altitude_ft;
    char callsign[9];
    /* decoders and the mesh: num as det_signal_t a,b (mesh.peer: 0 new, 1 lost) */
    uint32_t num[2];
    char origin[17], name[17], text[64];
    uint8_t mesh_note, remote;
    char reason[96];
} det_event_t;

typedef struct {
    bool quiet_on, quiet_mesh;
    int8_t quiet_start, quiet_end, utc_offset;
    uint8_t mesh_chan;        /* 1..3 */
    uint8_t mesh_rx_sinks;    /* the built-in rule for alerts other boards share */
} det_prefs_t;

/* The shared alert line, version 1, as the text of a group message on a
   private channel (MeshCore prefixes "name: "):
     #LSA/1|<origin>|<seq>|<source>|<flags>|<lat,lon>|<summary>
   origin the first 8 hex digits of the sender's key; seq 8 hex digits, a
   per-boot random half and a counter; source the sender's source bit index;
   flags E emergency, T test; lat,lon in 1e-4 degrees or empty; summary
   printable ASCII without '|', at most 60 characters. A newer version is
   counted, never misread. Received alerts are never forwarded. */
#define DET_ALERT_TAG "#LSA/"
#define DET_ALERT_SUMMARY 61
typedef struct {
    char origin[9];
    uint32_t seq;
    uint8_t source, flags;
    bool has_pos;
    int32_t lat_e4, lon_e4;
    char summary[DET_ALERT_SUMMARY];
} det_alert_t;
enum { DET_ALERT_F_EMERG = 1, DET_ALERT_F_TEST = 2 };
enum { DET_ALERT_OK = 0, DET_ALERT_NOT_ALERT, DET_ALERT_BAD, DET_ALERT_NEWER };
int det_alert_encode(const det_alert_t *m, char *out, size_t cap);
int det_alert_decode(const char *text, det_alert_t *out);
uint32_t det_alert_hash(const char *s);
/* Send policy: one a minute, the same summary not again within ten, the
   newest held while the minute runs. */
#define DET_ALERT_GAP_US (60LL * 1000000)
#define DET_ALERT_REPEAT_US (600LL * 1000000)
#define DET_ALERT_HOLD_US (300LL * 1000000)
typedef struct { int64_t next_at; uint32_t hash[8]; int64_t at[8]; unsigned i; } det_alert_gate_t;
enum { DET_ALERT_GO = 0, DET_ALERT_WAIT, DET_ALERT_REPEAT };
int det_alert_gate(det_alert_gate_t *g, uint32_t hash, int64_t now);
typedef struct { uint32_t origin[32], seq[32]; unsigned i; } det_alert_seen_t;
bool det_alert_seen(det_alert_seen_t *s, uint32_t origin, uint32_t seq);
enum { DET_NOTE_NONE = 0, DET_NOTE_SENT, DET_NOTE_HELD, DET_NOTE_REPEAT, DET_NOTE_TX_OFF,
       DET_NOTE_NO_CHANNEL, DET_NOTE_RELAYED, DET_NOTE_QUIET, DET_NOTE_FAILED };
const char *det_note_name(unsigned note);

typedef struct det det_t;
/* The sinks: which of `sinks` were taken (banner, sound, speech, vibration,
   the mesh, the SD log). */
typedef uint8_t (*det_deliver_fn)(det_t *d, det_event_t *e, uint8_t sinks);
struct det {
    det_rule_t rules[DET_RULES];
    unsigned n_rules;
    uint32_t revision;
    uint16_t sources;         /* the sources the operator allows */
    uint8_t sinks;            /* and the sinks */
    det_prefs_t prefs;
    int64_t utc;              /* wall clock for quiet hours, Unix seconds; 0 unknown */
    uint32_t observations, matches, suppressed, dropped;
    char error[96];
    det_event_t history[DET_HISTORY];   /* newest first */
    unsigned history_n;
    uint32_t sequence;
    det_deliver_fn deliver;
    void *user;
    /* private */
    struct { uint64_t entity; int64_t until; uint8_t rule; } cooldown[DET_COOLDOWNS];
    struct { int64_t until; uint32_t pending; } window[DET_RULES + 2];
    int64_t bucket_at;
    unsigned tokens;
    struct { uint32_t icao; int64_t at; } air_seen[DET_AIR_SLOTS];
    det_rule_t staging[DET_RULES];
    det_prefs_t staged_prefs;
    bool staged_share;
    unsigned root_seen;
    det_event_t work;
};

void det_init(det_t *d);
/* Replaces the rules; false keeps the old ones and says why in d->error. */
bool det_parse(det_t *d, const char *json, size_t len);
/* Applies what the operator may change in the session; false when invalid. */
bool det_rule_set(det_t *d, unsigned index, const det_rule_t *r);
bool det_prefs_set(det_t *d, const det_prefs_t *p);
bool det_quiet_at(bool on, int start, int end, int utc_offset, int64_t utc);
bool det_quiet(const det_t *d);
/* The board sources a lap_seen_watch should ask for: the allowed sources
   that an enabled rule uses (LoRa only then, as it starts a listener). */
uint32_t det_seen_mask(const det_t *d);
bool det_wants(const det_t *d, unsigned source);

void det_observe(det_t *d, const det_obs_t *o, int64_t now);
void det_drone_watch(det_t *d, const det_obs_t *o, int64_t now);
void det_signal(det_t *d, const det_signal_t *s, int64_t now);
/* Once per aircraft per second at most; stale ones the caller leaves out. */
void det_air(det_t *d, const det_air_t *a, int64_t now);
void det_test(det_t *d, int64_t now);

/* Aircraft. */
bool det_air_match(const det_rule_t *r, const det_air_t *a);
/* "A3": emitter set and number to a bit 0..15, or -1. */
int det_air_category_bit(const char *category);
const char *det_air_category_name(unsigned bit);
bool det_air_alarm(unsigned squawk, unsigned emergency);
void det_air_text(const det_air_t *a, char *out, size_t cap);
void det_air_event_text(const det_event_t *e, char *out, size_t cap);

/* Words. Never the intercepted text. */
const char *det_source_name(unsigned source);
const char *det_category_name(unsigned category);
const char *det_label(const det_event_t *e);
bool det_emergency(const det_event_t *e);
void det_describe(const det_event_t *e, char *out, size_t cap);
void det_speech(const det_event_t *e, char *out, size_t cap);
/* One JSONL record for the SD log: metadata only, no address or position. */
int det_log_line(const det_t *d, const det_event_t *e, uint64_t boot, char *out, size_t cap);
#endif
