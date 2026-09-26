/* Hits: moments a channel stood clear of its own noise, for FIND's log.

   Each track (a radio and one of its channels) keeps a noise floor from
   its quietest readings: the lowest of each 5 s over the last 40 s, and the
   middle of those. A transmitter that keys on and off (a beacon, a burst of
   packets) leaves quiet readings between, so however long it runs it never
   becomes its own floor; the middle, not the lowest, so one odd dip does not
   make everything look like a hit. The lowest reading sits a little under
   the average noise, and 2 dB is added back for it. A reading `threshold_db`
   over that floor opens a hit; readings over it within `gap_s` of the last
   one join it, and the hit keeps the strongest reading and the heading it
   was heard at. What made a hit doubtful travels with it as flags, so the
   operator can judge a row instead of trusting it.

   Pure: no radio, sensor or storage is touched here. */

#ifndef LS_DF_HITS_H
#define LS_DF_HITS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_DF_TRACKS   16
#define LS_DF_HIT_MAX  64
#define LS_DF_NOISE_BUCKETS 8
#define LS_DF_NOISE_BUCKET_US 5000000

enum {
    LS_DF_HIT_OVERLOAD = 1,   /* the receiver clipped during it */
    LS_DF_HIT_FAST     = 2,   /* turned faster than the receiver reads */
    LS_DF_HIT_FLAT     = 4,   /* the track's circle had no direction to it */
};

typedef struct {
    int64_t start_us, peak_us, last_us;
    uint32_t freq_hz;
    float peak;               /* strongest level, in the track's unit */
    float snr;                /* peak over the floor, dB */
    float heading;            /* true heading at the peak, NAN if none */
    uint16_t count;           /* readings that joined */
    uint8_t track;
    uint8_t flags;
} ls_df_hit_t;

typedef struct {
    float threshold_db;       /* over the floor to count */
    float gap_s;              /* quiet this long ends a hit */
} ls_df_hit_cfg_t;

typedef struct {
    ls_df_hit_t hit[LS_DF_HIT_MAX];
    int head, n;              /* ring: head is the next slot written */
    int open[LS_DF_TRACKS];   /* ring index of the track's open hit, -1 none */
    float noise[LS_DF_TRACKS];
    float quiet[LS_DF_TRACKS][LS_DF_NOISE_BUCKETS];   /* lowest reading in each bucket */
    int64_t bucket_us[LS_DF_TRACKS];                  /* when the current bucket began */
    uint8_t bucket[LS_DF_TRACKS], buckets[LS_DF_TRACKS];
    uint16_t heard[LS_DF_TRACKS];
    uint32_t total;
} ls_df_log_t;

void ls_df_log_clear(ls_df_log_t *g);
/* Forget one track's floor and open hit, after it is retuned. */
void ls_df_log_reset_track(ls_df_log_t *g, int track);
/* A reading. Returns the hit it joined or opened, NULL when it was noise. */
const ls_df_hit_t *ls_df_log_feed(ls_df_log_t *g, const ls_df_hit_cfg_t *cfg, int track,
                                  uint32_t freq_hz, float level, float heading,
                                  uint8_t flags, int64_t now_us);
float ls_df_log_noise(const ls_df_log_t *g, int track);
int ls_df_log_count(const ls_df_log_t *g);
/* 0 is the newest. */
const ls_df_hit_t *ls_df_log_at(const ls_df_log_t *g, int i);
/* Whether a hit is still growing: heard within the gap. */
bool ls_df_hit_open(const ls_df_hit_t *h, const ls_df_hit_cfg_t *cfg, int64_t now_us);

#ifdef __cplusplus
}
#endif
#endif
