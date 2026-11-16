/* BUGHUNT's tag watch: what the tracker detector knows, without the screen
   (it began as the BLE WATCH app, now folded into BUGHUNT). The app feeds
   advertising reports in; this keeps the device table, names the trackers,
   groups rotating addresses and decides when one is following.
   Receive only. Pure C with no board calls, so the bench tests it directly.

   Its public names get a bh_ prefix (below), so they cannot meet another
   app's in the simulator, which links every app into one program. */
#ifndef BH_TAGS_H
#define BH_TAGS_H
#define bw_init       bh_bw_init
#define bw_parse      bh_bw_parse
#define bw_add        bh_bw_add
#define bw_position   bh_bw_position
#define bw_tick       bh_bw_tick
#define bw_find_uid   bh_bw_find_uid
#define bw_window     bh_bw_window
#define bw_presence   bh_bw_presence
#define bw_hist       bh_bw_hist
#define bw_trend      bh_bw_trend
#define bw_span_ms    bh_bw_span_ms
#define bw_alertable  bh_bw_alertable
#define bw_class_tag  bh_bw_class_tag
#define bw_class_name bh_bw_class_name
#define bw_class_note bh_bw_class_note
#define bw_company    bh_bw_company
#define bw_pdu_name   bh_bw_pdu_name
#define bw_addr_kind  bh_bw_addr_kind
#define bw_addr_text  bh_bw_addr_text
#define bw_label      bh_bw_label
#define bw_distance_m bh_bw_distance_m
#define bw_csv        bh_bw_csv
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BW_MAX_DEV 128
#define BW_HIST 60              /* RSSI buckets kept per device */
#define BW_HIST_MS 2000         /* one bucket: two minutes in all */
#define BW_MINUTES 16           /* per-minute sighting counts kept */
#define BW_AD_KEEP 40           /* bytes of the last advert kept for the detail view */
#define BW_NO_RSSI (-128)

/* What a device is. Order matters only for the tables in bh_tags.c. */
enum {
    BW_C_NONE = 0,
    BW_C_FINDMY,        /* Apple Find My offline finding (AirTag, AirPods, third-party Find My tags) */
    BW_C_APPLE_PP,      /* Apple proximity pairing (AirPods out of the case, AirTag setup) */
    BW_C_TILE,
    BW_C_SMARTTAG,      /* Samsung Galaxy SmartTag offline finding */
    BW_C_FMDN,          /* Google Find My Device network tag */
    BW_C_DULT,          /* tag in DULT separated mode (the Apple/Google cross-platform spec) */
    BW_C_CHIPOLO,
    BW_C_FASTPAIR,      /* Google Fast Pair: headphones, some tags */
    BW_C_IBEACON,
    BW_C_EDDYSTONE,
    BW_C_APPLE,         /* other Apple continuity traffic (phones, watches, Macs) */
    BW_C_MICROSOFT,
    BW_C_COUNT
};

/* One advertising report, parsed. */
typedef struct {
    uint16_t company;           /* manufacturer data company ID; 0xFFFF none */
    uint16_t uuid16[6];         /* 16-bit service UUIDs, listed or with service data */
    uint8_t n_uuid;
    uint8_t flags;              /* AD type 0x01 */
    bool has_flags, has_tx;
    int8_t tx_power;
    uint16_t appearance;
    char name[24];
    uint8_t cls;                /* BW_C_* */
    uint8_t level;              /* 0 not a tracker, 1 possible, 2 tracker */
    uint8_t sub;                /* Apple continuity type, Eddystone frame type ... */
    uint8_t status;             /* Find My status byte */
    int8_t separated;           /* 1 separated from its owner, 0 near it, -1 unknown */
    uint32_t model;             /* Apple proximity pairing model; Fast Pair 24-bit model ID */
} bw_ad_t;

typedef struct {
    uint32_t uid;               /* never reused; links rotations */
    uint8_t addr[6];            /* as the radio gives it: little endian */
    uint8_t addr_type, pdu;     /* 0 public 1 random; LAP_BLE_* PDU type, 0xFF unknown */
    bw_ad_t ad;
    uint8_t raw[BW_AD_KEEP], raw_len;
    uint32_t first_ms, last_ms;     /* this address */
    uint32_t chain_ms;              /* first sight of the chain (through rotations), reset after a long gap */
    uint32_t count;
    struct { uint32_t minute; uint16_t n; } mins[BW_MINUTES];
    int8_t hist[BW_HIST];           /* strongest RSSI per bucket */
    uint32_t hist_at;               /* bucket number of hist[BW_HIST-1] */
    float smooth;
    int8_t rssi, peak;
    uint32_t pred, succ;            /* uids either side of a rotation; 0 none */
    uint16_t rotations;
    bool chain_checked;
    bool has_pos0;
    double lat0, lon0;
    float moved_m;
    uint8_t alert;                  /* 1 once the following alert has fired */
    uint32_t alert_ms;
} bw_dev_t;

typedef struct {
    bw_dev_t dev[BW_MAX_DEV];
    int n;
    uint32_t next_uid;
    uint32_t follow_ms;         /* present this long ... */
    float follow_m;             /* ... and moved this far when there is a fix */
    uint32_t gap_ms;            /* unseen longer than this starts presence again */
    bool alert_possible;        /* also alert on level-1 (possible) trackers */
    bool fix;
    double lat, lon;
    bool ever_fix;              /* a fix was seen since the app opened */
    uint32_t adverts, evicted;
} bw_t;

void bw_init(bw_t *w);
/* Parse the AD structures of one report. False when they do not parse;
   whatever was read before the fault is kept. */
bool bw_parse(const uint8_t *ad, size_t n, bw_ad_t *out);
/* One report in. Returns the device's index. */
int bw_add(bw_t *w, const uint8_t addr[6], uint8_t addr_type, uint8_t pdu, int rssi, const uint8_t *ad, size_t n,
           uint32_t now_ms);
/* The current position, when there is one; distances use it. */
void bw_position(bw_t *w, bool fix, double lat, double lon);
/* Settle rotations and alerts. Returns the index of a device whose following
   alert fired just now, or -1. Call it often; it reports one at a time. */
int bw_tick(bw_t *w, uint32_t now_ms);

/* Reading the table. */
int bw_find_uid(const bw_t *w, uint32_t uid);
int bw_window(const bw_dev_t *d, uint32_t now_ms, int minutes);      /* sightings in the last N minutes */
uint32_t bw_presence(const bw_dev_t *d, uint32_t now_ms);           /* bit i: heard in the minute i ago (0..15) */
void bw_hist(const bw_dev_t *d, uint32_t now_ms, int8_t out[BW_HIST]); /* oldest first, BW_NO_RSSI empty */
int bw_trend(const bw_dev_t *d, uint32_t now_ms);                    /* +1 stronger, -1 weaker, 0 steady */
uint32_t bw_span_ms(const bw_dev_t *d);                              /* how long the chain has been about */
bool bw_alertable(const bw_t *w, const bw_dev_t *d);
const char *bw_class_tag(int cls);      /* short, for lists */
const char *bw_class_name(int cls);     /* long, for the detail view */
const char *bw_class_note(int cls);     /* one line of advice, or "" */
const char *bw_company(uint16_t id);    /* NULL when not in the table */
const char *bw_pdu_name(uint8_t pdu);
const char *bw_addr_kind(const uint8_t addr[6], uint8_t addr_type);
void bw_addr_text(const uint8_t addr[6], char out[18]);
const char *bw_label(const bw_dev_t *d, char *buf, size_t cap);
float bw_distance_m(double lat1, double lon1, double lat2, double lon2);
/* One CSV log line (no newline added when cap is short). Columns:
   uptime_s,event,address,addr_type,class,company,rssi,sightings_5m,span_s,moved_m,rotations,name */
int bw_csv(const bw_dev_t *d, const char *event, uint32_t now_ms, char *out, size_t cap);
#define BW_CSV_HEADER "uptime_s,event,address,addr_type,class,company,rssi,sightings_5m,span_s,moved_m,rotations,name\n"
#endif
