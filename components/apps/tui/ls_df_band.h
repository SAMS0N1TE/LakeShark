/* Finding what is on a band, not only what was tuned to.

   The SX1262 sweeps a span of up to a few tens of MHz, 64 steps a pass.
   Each step learns what it usually hears: a running mean and spread of its
   level, taken only while nothing stands out there. A step is on the air
   when it stands well over that (four spreads and 6 dB), or when it stands
   over the band's own floor by LS_DFB_LOUD_DB, which catches a transmitter
   that never stops and so became part of the learnt background.

   A run of raised steps is one emitter at its loudest step: a strong
   transmitter close by lifts the receiver's floor for megahertz around it,
   and that skirt is not a crowd. A new emitter must stand out on two passes
   within 3 s before it takes a track.

   Each emitter found gets one of LS_DFB_TRACKS tracks and keeps it while it
   is heard; the frequency is fixed when the track opens so what FIND has
   learnt about its direction stays with it. A track follows its emitter a
   step either way and keeps it with a lower bar (3 dB and three spreads), so the directions
   where the body shadows it still count. A track unheard for
   LS_DFB_FORGET_US is free again, the stalest first when a new one needs a
   place.

   Pure: no radio is touched here. ls_df_sources.c feeds it. */

#ifndef LS_DF_BAND_H
#define LS_DF_BAND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_DFB_BINS      64
#define LS_DFB_TRACKS    8
#define LS_DFB_WARM      8           /* passes learnt before anything is judged */
#define LS_DFB_LOUD_DB   12.0f
#define LS_DFB_FORGET_US 120000000
#define LS_DFB_PENDING   8

typedef struct {
    bool live;
    uint8_t bin;                     /* where it is heard now */
    uint32_t hz;                     /* where it was first heard */
    float level, peak, over;         /* latest, strongest, latest over background */
    int64_t first_us, last_us;
    uint32_t passes;                 /* passes it was on the air */
} ls_dfb_track_t;

typedef struct {
    uint32_t lo_hz, hi_hz;
    float mean[LS_DFB_BINS], var[LS_DFB_BINS];
    float floor;                     /* the latest pass's quiet level */
    uint32_t passes, opened;
    ls_dfb_track_t track[LS_DFB_TRACKS];
    uint8_t pend_bin[LS_DFB_PENDING];   /* strong once, waiting for a second pass */
    int64_t pend_us[LS_DFB_PENDING];
} ls_dfb_t;

typedef struct { uint8_t track; uint32_t hz; float level; } ls_dfb_read_t;

void ls_dfb_reset(ls_dfb_t *b, uint32_t lo_hz, uint32_t hi_hz);
uint32_t ls_dfb_bin_hz(const ls_dfb_t *b, int bin);
/* One pass of LS_DFB_BINS levels in dBm, low frequency first. Returns the
   readings for the tracks on the air in this pass, at most `max`. */
int ls_dfb_pass(ls_dfb_t *b, const float dbm[LS_DFB_BINS], int64_t now_us, ls_dfb_read_t *out, int max);

#ifdef __cplusplus
}
#endif
#endif
