/* Radio direction finding from signal strength and a compass.

   Turn slowly on the spot while a receiver reports a level. Each level is
   filed under the heading it was heard at; the strongest direction is the
   bearing (PEAK), or, with the board held against the chest so the body
   shadows the antenna, the weakest direction points away from it (NULL).
   Bearings kept from two or more places cross at the transmitter.

   Pure: no radio and no sensor is touched here. ls_df_sources.c feeds it. */

#ifndef LS_DF_H
#define LS_DF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_DF_BINS     72          /* 5 degrees each */
#define LS_DF_BEARINGS 8

typedef enum { LS_DF_PEAK, LS_DF_NULL } ls_df_method_t;

typedef struct {
    float level[LS_DF_BINS];       /* held level: the strongest, until it decays */
    float last[LS_DF_BINS];        /* the latest level heard in the bin */
    uint16_t count[LS_DF_BINS];
    int64_t  seen_us[LS_DF_BINS];
    int64_t  peak_us[LS_DF_BINS];  /* when the held level was heard */
    int64_t  decayed_us;
    float floor, top;              /* weakest and strongest bin with data */
    uint32_t samples;
} ls_df_sweep_t;

/* How a held level lets go. It stays hold_s after it was heard, then falls
   decay_db_s a second, never below the latest level heard that way. A
   decay of 0 holds forever. */
typedef struct { float hold_s, decay_db_s; } ls_df_decay_t;

/* A local maximum of the smoothed levels. Prominence is how far it stands
   above the lowest ground between it and anything higher. */
typedef struct { float bearing, level, prominence; } ls_df_peak_t;

typedef struct {
    bool valid;
    float bearing;                 /* true degrees, where the signal is */
    float spread;                  /* +/- degrees: half the lobe at 3 dB */
    float contrast;                /* strongest minus weakest, dB */
    int   coverage;                /* degrees of the circle with data */
} ls_df_estimate_t;

typedef struct {
    double lat, lon;
    float bearing, spread, level;
    int64_t time_us;
    char source[16];
} ls_df_bearing_t;

typedef struct {
    bool valid;
    double lat, lon;
    float radius_m;                /* rough 1-sigma of the crossing */
    int used;
} ls_df_fix_t;

void ls_df_clear(ls_df_sweep_t *s);
/* heading in true degrees, level in dB (any consistent unit). */
void ls_df_add(ls_df_sweep_t *s, float heading, float level, int64_t now_us);
/* Bins older than max_age_us are forgotten, so a sweep follows a walk. */
void ls_df_age(ls_df_sweep_t *s, int64_t now_us, int64_t max_age_us);
bool ls_df_estimate(const ls_df_sweep_t *s, ls_df_method_t method, ls_df_estimate_t *out);
void ls_df_decay(ls_df_sweep_t *s, int64_t now_us, const ls_df_decay_t *d);
/* Up to max peaks at least min_prominence_db tall, strongest first; true
   bearings of the raw lobe, no NULL flip and no calibration. */
int  ls_df_peaks(const ls_df_sweep_t *s, float min_prominence_db, ls_df_peak_t *out, int max);

/* Power of the signal in a narrow band of unsigned 8-bit interleaved IQ,
   as an SDR delivers it: `offset_hz` from the tuned centre, `bw_hz` wide,
   in dB relative to full scale. The band is summed from single DFT bins,
   which for the few bins a channel spans costs less than an FFT. */
float ls_df_iq_band_db(const uint8_t *iq, int pairs, uint32_t rate_hz,
                       int32_t offset_hz, uint32_t bw_hz);

/* Least squares crossing of bearing lines, on a local flat projection;
   needs two bearings at least 15 degrees apart. */
bool ls_df_triangulate(const ls_df_bearing_t *b, int n, ls_df_fix_t *out);

#ifdef __cplusplus
}
#endif
#endif
