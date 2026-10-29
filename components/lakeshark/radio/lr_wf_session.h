#ifndef LR_WF_SESSION_H
#define LR_WF_SESSION_H
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* UI holds the radio until stop returns. Owner confirms each tune by receiving
 * checked blocks; center remains zero until then. start is nonblocking. */
esp_err_t lr_wf_start(uint32_t hz);
/* Single owner invokes consumer before releasing each checked block. Never
 * block here. NULL/0 signals a discontinuity and requires a full DSP reset.
 * Consumer registration is immutable until stop returns. */
typedef void (*lr_wf_consumer_t)(void *, const uint8_t *, size_t, bool spare);
esp_err_t lr_wf_start_consumer(uint32_t hz, lr_wf_consumer_t consume, void *ctx);
typedef struct { uint32_t lost_pairs, gaps; bool done; esp_err_t error; } lr_wf_stats;
void lr_wf_get_stats(lr_wf_stats *out);
void lr_wf_stop_stats(lr_wf_stats *out);
void lr_wf_stop(void);

/* Wide mode, off by default. The LO sits offset_hz below the channel, the
 * 576k IQ8 route streams, and a down-converter in the owner task hands the
 * consumer and the waterfall the same 125k IQ16 they get today, at most 256
 * pairs a call, with the channel at 0 Hz: the LO's DC leakage and centre
 * spurs land offset_hz away, outside the channel. A running session moves
 * to the new setting by restarting its stream (the consumer sees one NULL/0
 * reset); a retune already restarts it. Any change clears the counters. */
#define LR_WF_WIDE_OFFSET_HZ     100000   /* a general offset (oscillator mix) */
/* The setting at boot: 576k/4, where lr_ddc mixes by sign and swap instead
   of an oscillator, its cheapest case. */
#define LR_WF_WIDE_DEFAULT_HZ    144000
#define LR_WF_WIDE_OFFSET_MIN_HZ 10000
#define LR_WF_WIDE_OFFSET_MAX_HZ 225000   /* 576k/2 - 125k/2: lr_ddc's limit */
/* |offset_hz| outside MIN..MAX is ESP_ERR_INVALID_ARG; the sign picks the side. */
esp_err_t lr_wf_set_wide(bool on, int32_t offset_hz);
/* The setting alone, cheap enough to poll per block. */
bool lr_wf_wide_on(void);
/* An owner that wants wide whatever the setting (`p25 lrwide on`) pins it
   for its session and clears the pin when it stops; the setting and its
   counters are untouched. Set it before start, or the next run switches. */
void lr_wf_set_wide_force(bool on);
typedef struct {
    bool on; int32_t offset_hz;          /* the setting                        */
    bool active, wide;                   /* a stream runs; that run is wide    */
    uint32_t channel_hz, lo_hz, rate_hz; /* the running stream, 0 when none   */
    uint32_t ddc_mcycles, expand_mcycles;/* per input pair x1000, last ~1 s    */
    uint32_t queue_slots, queue_high_water, queue_drops;
    uint32_t fifo_overflows, fifo_full;  /* FIFO flag overflows, level-full ends */
    uint32_t runs, switches;             /* wide streams started, in-session moves */
    uint32_t in_pairs, out_pairs, ddc_dropped;
    /* The last wide run that lost samples: when (us into the run), the
       worst gap between FIFO reads, the worst wake lateness, the longest
       drain and the peak FIFO level. All 0 until one does. */
    uint32_t bad_at_us, bad_drain_gap_us, bad_wake_late_us, bad_drain_us, bad_peak_fifo;
    uint32_t bad_level_us, bad_read_us, bad_publish_us, bad_agc_us; /* worst single steps */
    uint32_t bad_busy_us;                /* worst BUSY wait inside a read      */
    /* In-session retunes (lr_wf_retune): how many, and the request-to-first-
       block time of the last and the slowest, narrow or wide. */
    uint32_t retunes, retune_us, retune_max_us;
} lr_wf_wide_status;
void lr_wf_wide_get(lr_wf_wide_status *out);

/* Move the running session to hz without ending it: the capture stops, the
 * consumer sees one NULL/0 reset, and the next block is the new channel.
 * Narrow re-programs the modem there; wide only moves the LO and keeps the
 * down-converter's filter, so a hop costs one chip recovery either way and
 * wide skips the modem's re-program. For trunking (P25 grant follow).
 * ESP_ERR_INVALID_STATE without a live session, INVALID_ARG off the band. */
esp_err_t lr_wf_retune(uint32_t hz);

/* One observer of the checked blocks the owner hands out, narrow or wide:
 * 125k IQ16 with the channel at 0 Hz. Called on the owner task after the
 * consumer; it must only copy what it wants and return. NULL removes it. */
typedef void (*lr_wf_tap_t)(const uint8_t *iq16, size_t bytes, uint32_t channel_hz);
void lr_wf_set_tap(lr_wf_tap_t tap);
/* The capture tap, separate from the RF LAB tap above: a copy of every block
   the consumer gets (125k IQ16, the channel at 0 Hz), on the owner task, for
   TIME MACHINE (ls_tmach.h): hz is the channel, and NULL/0 is the consumer's
   own discontinuity. It must copy and return. NULL removes it. */
typedef void (*lr_wf_capture_tap_t)(uint32_t hz, const uint8_t *iq16, size_t bytes);
void lr_wf_set_capture_tap(lr_wf_capture_tap_t tap);
#ifdef __cplusplus
}
#endif
#endif
