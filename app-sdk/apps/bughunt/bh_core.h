/* BUGHUNT: what the sweep knows, without the screen. The app (main.c) feeds
   in RSSI samples from the LoRa radio, Bluetooth adverts and Wi-Fi frames;
   this keeps the band plan, max-hold per step and per time slot, bursts,
   carriers, tags and stations, and turns them into ranked verdicts.
   Receive only. Pure C with no board calls, so the bench tests it directly. */
#ifndef BH_CORE_H
#define BH_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bh_tags.h"

#define BH_MAX_STEPS 128
#define BH_MAX_BANDS 12
#define BH_SLOTS 96             /* the sweep's timeline, start to end */
#define BH_MAX_BURSTS 40        /* per band */
#define BH_MAX_STA 48
#define BH_MAX_FIND 24
#define BH_NONE (-32768)        /* no sample (tenths of a dBm) */
#define BH_DIRS 36              /* direction plot: 10 degrees a bin */

enum { BH_ROOM = 0, BH_CAR = 1 };
enum { BH_REG_US = 1, BH_REG_AR = 2, BH_REG_BOTH = 3 };
enum { BH_BAND_CELL = 1, BH_BAND_ISM, BH_BAND_UHF, BH_BAND_24G };

/* Verdict kinds, roughly in order of how much they matter. */
enum {
    BH_F_CELL_PERIODIC = 1,     /* cellular uplink burst at a steady interval: tracker check-in */
    BH_F_BLE_FOLLOW,            /* tracker tag heard the whole sweep */
    BH_F_WIFI_STREAM,           /* station uploading steadily (video-like) */
    BH_F_CARRIER,               /* continuous carrier outside the cell bands */
    BH_F_STRONG,                /* very strong signal: a transmitter close by */
    BH_F_ISM_PERIODIC,          /* ISM burst at a steady interval */
    BH_F_WIFI_CAM,              /* camera maker's Wi-Fi device on air */
    BH_F_BLE_TAG,               /* tracker tag heard, not the whole sweep */
    BH_F_PHONE,                 /* irregular cellular bursts: a phone in use */
    BH_F_CELL_BURSTS,           /* a few cellular bursts, no pattern yet */
};
enum { BH_T_FREQ = 1, BH_T_BLE, BH_T_WIFI };

typedef struct {
    uint32_t t_ms, end_ms;      /* first and last hot sample */
    int16_t peak;               /* tenths of a dBm */
    uint16_t step;
} bh_burst_t;

typedef struct {
    char name[8];               /* short label for the timeline: "B28", "433" */
    char what[28];              /* "LTE uplink 703-748" */
    uint8_t kind;               /* BH_BAND_* */
    uint16_t first, count;      /* its steps */
    uint32_t lo_khz, hi_khz;
    int16_t peak;               /* strongest sample this sweep */
    uint16_t nburst;
    bh_burst_t burst[BH_MAX_BURSTS];
    uint32_t hot_ms;            /* last hot sample, 0 none */
    bool failed;                /* the radio refused its frequencies */
} bh_band_t;

typedef struct {
    uint32_t khz;
    uint8_t band;
    int16_t floor;              /* quiet level: falls fast, rises slowly */
    int16_t peak, last;
    uint16_t visits, hot;       /* samples, and samples well over the floor */
    int16_t dev;                /* how far quiet samples sit over the floor (tenths, averaged) */
    uint32_t last_ms;
} bh_step_t;

typedef struct {
    uint8_t mac[6];
    uint8_t vendor;             /* index into the camera table + 1; 0 none */
    int8_t rssi, peak;
    uint32_t first_ms, last_ms;
    uint32_t frames, up_frames, up_bytes;
    uint16_t buckets, buckets_on;   /* 10 s buckets seen, and those with a steady upload */
    uint32_t bucket_at, bucket_bytes;
    char maker[24];             /* from the name file (bh_lookup), "" not known */
    bool cam_maker;             /* that maker makes cameras first */
} bh_sta_t;

typedef struct {
    uint8_t kind;               /* BH_F_* */
    uint8_t conf;               /* 0..100 */
    uint8_t target;             /* BH_T_*: what HUNT parks on */
    uint8_t band;
    uint32_t khz;
    uint8_t addr[6], addr_type;
    uint32_t uid;               /* BLE chain id */
    int16_t level;              /* tenths of a dBm (BLE/Wi-Fi: dBm * 10) */
    uint32_t period_ms;
    uint16_t count;
    char title[48];
    char detail[200];
} bh_find_t;

typedef struct {
    uint8_t mode, region;
    bool has_24g;
    uint32_t t0_ms, dur_ms, slot_ms;
    uint32_t sweep_ms;          /* time for one pass of the whole plan (the app measures it) */
    int nband, nstep;
    bh_band_t band[BH_MAX_BANDS];
    bh_step_t step[BH_MAX_STEPS];
    int16_t slot[BH_MAX_BANDS][BH_SLOTS];       /* max per band per time slot */
    int16_t hslot[BH_MAX_BANDS][BH_SLOTS];      /* most over a step's hot threshold (<0: none hot) */
    int16_t sslot[BH_MAX_STEPS][BH_SLOTS];      /* max per step per time slot */
    int16_t ref;                /* the plan's quiet level: median of the step floors */
    int16_t qtop;               /* the top of the noise: median hot threshold (for drawing) */
    int16_t bqtop[BH_MAX_BANDS];    /* the same per band: a tower's steady downlink stays under it */
    uint32_t samples, last_sample_ms;
    bh_sta_t sta[BH_MAX_STA];
    int nsta;
    uint32_t wifi_frames, ble_adverts;
    bw_t ble;
} bh_t;

/* A new sweep: the band plan for mode and region (plus 2.4 GHz when the
   radio reaches it), all counters cleared. dur_ms is how long it runs. */
void bh_init(bh_t *h, int mode, int region, bool has_24g, uint32_t dur_ms, uint32_t now_ms);
/* The plan in scan lists of at most `max` frequencies (the radio's scanner
   takes 64). Returns how many in chunk k; 0 past the end. */
int bh_chunks(const bh_t *h, int max);
int bh_chunk(const bh_t *h, int k, int max, uint64_t *hz);
int bh_step_of(const bh_t *h, uint64_t hz);     /* -1 when not in the plan */
/* One RSSI sample in tenths of a dBm. */
void bh_rf_sample(bh_t *h, uint64_t hz, int tenths, uint32_t now_ms);
/* The radio would not tune a frequency: its band is left out. */
void bh_rf_refused(bh_t *h, uint64_t hz);
/* One Bluetooth advertising report (the AD structures). */
void bh_ble_advert(bh_t *h, const uint8_t addr[6], uint8_t addr_type, uint8_t pdu, int rssi, const uint8_t *ad,
                   size_t n, uint32_t now_ms);
/* One 802.11 frame as lap_capture_read gives it; orig_len is its size on air. */
void bh_wifi_frame(bh_t *h, const uint8_t *frame, size_t n, size_t orig_len, int rssi, uint32_t now_ms);
/* Settle the floors' median and the tag table; call a few times a second. */
void bh_tick(bh_t *h, uint32_t now_ms);
/* Verdicts, most important first. */
int bh_findings(bh_t *h, uint32_t now_ms, bh_find_t *out, int max);

/* Reading. */
int bh_slot_now(const bh_t *h, uint32_t now_ms);
bool bh_step_carrier(const bh_t *h, int step);
int bh_hot_threshold(const bh_t *h, int step);  /* tenths of a dBm */
const char *bh_cam_vendor(const uint8_t mac[6]);    /* NULL when not a camera maker */
/* The name file named station i's maker; camera: a camera maker. */
void bh_sta_maker(bh_t *h, int i, const char *name, bool camera);
/* A steady interval in event times t[0..n-1] (rising), allowing missed events
   and `jitter_ms` of timing error. Returns the period in ms, 0 none; *matched
   gets how many intervals fit it. */
uint32_t bh_period(const uint32_t *t, int n, uint32_t jitter_ms, int *matched);

/* HUNT: the strongest level heard per heading, for the turn-in-a-circle plot. */
typedef struct {
    int16_t best[BH_DIRS];      /* tenths of a dBm, BH_NONE unvisited */
    int16_t peak;
    int peak_bin;
} bh_dir_t;
void bh_dir_reset(bh_dir_t *d);
void bh_dir_add(bh_dir_t *d, float heading_deg, int tenths);
int bh_dir_covered(const bh_dir_t *d);          /* bins visited */
/* Geiger: milliseconds between ticks for a level over a floor (both tenths). */
uint32_t bh_geiger_ms(int tenths, int floor_tenths);
#endif
