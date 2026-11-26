/* BUGHUNT's finder maths, without the screen: what HUNT does with the
   readings it gets. Pure C with no board calls, so the bench tests it.

   - bh_lvl_t: the level to show. Bluetooth fades by 10-20 dB from one advert
     to the next as you move (the signal cancels itself off walls and your
     body), so the level is the strongest reading of the last moment, not the
     latest one; a frequency is read many times a second and keeps a shorter
     window. Fast and slow averages of that give HOTTER / COLDER.
   - bh_range_t: the meter's range steps up 20 dB at a time as you close in,
     so the bar never sits pinned at the top next to the thing.
   - bh_dist_*: how far a tag is, from its level and what a tag of its kind
     reads at 1 m (or what you measured with CAL 1 M).
   - bh_dir2_t: direction from turning in a circle. Each 10 degrees keeps a
     smoothed level and the neighbours are blended, so one lucky reading
     does not win. PEAK points at the loudest heading; BODY uses your body
     as the shield (hold the board to your chest, turn: the quietest heading
     is your back to the thing, so it is behind you).
   - bh_bursts_t: a tracker that checks in every minute or two is silent
     between bursts. Each burst is kept with its peak and the heading it came
     from, and the spacing gives when the next one is due.
   - bh_pair_t: two receivers on one frequency (DUAL): a directional antenna
     on one and the other as the reference. Their difference does not move
     when the transmitter's own level changes burst to burst, only when the
     directional antenna turns toward or away from it. */
#ifndef BH_HUNT_H
#define BH_HUNT_H
#include <stdbool.h>
#include <stdint.h>

#define BH_HNONE (-32768)

/* ---- the level ---- */
#define BH_LVL_N 48
typedef struct {
    int16_t v[BH_LVL_N];        /* tenths of a dBm */
    uint32_t t[BH_LVL_N];
    int n, head;
    uint32_t window_ms;         /* strongest reading over this long */
    float fast, slow;           /* averages of the windowed level; BH_HNONE-ish until the first */
    bool started;
    int16_t peak;               /* strongest since the hunt began */
    uint32_t last_ms;
} bh_lvl_t;
void bh_lvl_init(bh_lvl_t *l, uint32_t window_ms);
void bh_lvl_add(bh_lvl_t *l, int tenths, uint32_t now_ms);
/* The strongest reading within the window, BH_HNONE when nothing is that fresh. */
int bh_lvl_now(const bh_lvl_t *l, uint32_t now_ms);
/* +1 getting stronger, -1 weaker, 0 steady (fast vs slow, dB). */
int bh_lvl_trend(const bh_lvl_t *l, int db);

/* ---- the meter's range ---- */
typedef struct {
    int16_t base;               /* the floor the range starts from (tenths) */
    int16_t span;               /* tenths a full bar covers */
    uint8_t step;               /* 0, 1, 2 ...: 20 dB each */
    uint8_t max_step;
    uint32_t over_ms, under_ms; /* since when the level sat at the top / low in the range */
} bh_range_t;
void bh_range_init(bh_range_t *r, int base_tenths, int span_tenths, int max_step);
/* Feed the windowed level; steps up after 600 ms pinned at the top and down
   after 3 s in the bottom quarter. Returns +1 / -1 when it stepped. */
int bh_range_feed(bh_range_t *r, int tenths, uint32_t now_ms);
int bh_range_lo(const bh_range_t *r);           /* tenths, the left end */
int bh_range_hi(const bh_range_t *r);
float bh_range_frac(const bh_range_t *r, float tenths);    /* 0..1 within the range */

/* ---- how far a tag is ---- */
/* What a tag of this kind reads at 1 m (dBm), from its advertised TX power
   when it sends one (TX power is given at 0 m; 1 m at 2.4 GHz is ~41 dB less). */
int bh_dist_ref(int cls, bool has_tx, int tx_power);
/* metres for a level, the 1 m reference and the path-loss exponent (2 open
   air, 2.5-3 through a house). */
float bh_dist_m(int rssi_dbm, int ref1m_dbm, float n);
/* "0.4 m", "3 m", "12 m" with the likely span either side (a 6 dB fade). */
void bh_dist_text(int rssi_dbm, int ref1m_dbm, char *out, int cap);

/* ---- direction ---- */
#define BH_DIR2 36
enum { BH_DM_PEAK = 0, BH_DM_BODY = 1 };
typedef struct {
    int16_t val[BH_DIR2];       /* smoothed level per 10 degrees, BH_HNONE unvisited */
    uint16_t hits[BH_DIR2];
    int16_t lo, hi;             /* weakest and strongest smoothed value */
} bh_dir2_t;
typedef struct {
    int bearing;                /* degrees 0..359 where the thing is, -1 not yet */
    int contrast;               /* tenths of a dB: how much the answer stands out */
    int width;                  /* degrees of the lobe (within 3 dB of the best) */
    int covered;                /* bins visited */
    int quality;                /* 0 none, 1 unclear, 2 broad, 3 sharp */
} bh_dir_fix_t;
void bh_dir2_reset(bh_dir2_t *d);
void bh_dir2_add(bh_dir2_t *d, float heading_deg, int tenths);
/* The blended value of bin b (its neighbours count half), BH_HNONE unvisited. */
int bh_dir2_bin(const bh_dir2_t *d, int b);
bh_dir_fix_t bh_dir2_fix(const bh_dir2_t *d, int mode);
/* Signed turn from where you face to a bearing: -179..180, + is right. */
int bh_turn(int facing_deg, int bearing_deg);

/* ---- bursts ---- */
#define BH_BURSTS 16
typedef struct {
    uint32_t t_ms, dur_ms;
    int16_t peak;
    int16_t heading;            /* degrees at the peak, -1 unknown */
} bh_hburst_t;
typedef struct {
    bh_hburst_t b[BH_BURSTS];   /* oldest first */
    int n;
    bool in;                    /* a burst is going on */
    bh_hburst_t cur;
    uint32_t quiet_ms;          /* last reading under the threshold */
    int16_t thr;                /* tenths: over this is a burst */
    uint32_t period_ms;         /* 0 none yet */
    uint32_t total;
} bh_bursts_t;
void bh_bursts_init(bh_bursts_t *b, int floor_tenths);
/* One reading. Returns true when a burst just ended (it is b->b[b->n-1]). */
bool bh_bursts_add(bh_bursts_t *b, int tenths, int heading_deg, uint32_t now_ms);
/* A burst that has not ended after 400 ms quiet ends here too. */
bool bh_bursts_tick(bh_bursts_t *b, uint32_t now_ms);
/* ms until the next burst is due (0 overdue), -1 no period. */
int32_t bh_bursts_due(const bh_bursts_t *b, uint32_t now_ms);
/* The last burst against the one before, tenths of a dB (+ stronger). */
int bh_bursts_delta(const bh_bursts_t *b);
/* True when bursts come and go (not one steady carrier). */
bool bh_bursts_bursty(const bh_bursts_t *b, uint32_t now_ms);

/* ---- two receivers ---- */
typedef struct {
    bh_lvl_t a, b;              /* a: the directional (DIR), b: the reference (REF) */
    int16_t floor_a, floor_b;   /* each receiver's quiet level */
    float diff;                 /* smoothed (a over its floor) - (b over its floor), tenths */
    bool have;
    float best;                 /* the most the difference has been */
} bh_pair_t;
void bh_pair_init(bh_pair_t *p, uint32_t window_ms);
void bh_pair_floor(bh_pair_t *p, int floor_a, int floor_b);
void bh_pair_add(bh_pair_t *p, int which, int tenths, uint32_t now_ms);
/* The pointing difference in tenths of a dB, BH_HNONE when either is stale. */
int bh_pair_point(bh_pair_t *p, uint32_t now_ms);
#endif
