#ifndef LS_UAT_STREAM_H
#define LS_UAT_STREAM_H
#include "uat_decode.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UAT_SAMPLE_RATE 2083333u
#define UAT_DOWNLINK_SYNC UINT64_C(0xEACDDA4E2)
#define UAT_UPLINK_SYNC UINT64_C(0x153225B1D)

typedef void (*uat_downlink_fn)(const uat_fix_t *, void *);
typedef void (*uat_uplink_fn)(const uint8_t payload[432], void *);
typedef struct {
    uint64_t samples;
    uint32_t sync_down, sync_up, short_frames, long_frames, uplink_frames;
    uint32_t bad_frames, corrected, duplicates;
} uat_stats_t;
typedef struct {
    uint64_t shift;
    uint8_t frame[552];
    unsigned bits, sync_bits;
    bool capturing, uplink;
} uat_phase_t;
/* Caller owns this 1.3 KB state; place it in PSRAM on the device. Feed on
   one decoder task, including across arbitrarily split IQ transfers. */
typedef struct {
    uat_phase_t phase[2];
    uat_stats_t stats;
    int prev_i, prev_q, previous_metric;
    uint8_t pending_i;
    bool have_i, have_previous, accepted_up, have_accepted;
    uint64_t accepted_sample;
    uat_downlink_fn downlink;
    uat_uplink_fn uplink;
    void *user;
} uat_stream_t;
void uat_stream_init(uat_stream_t *, uat_downlink_fn, uat_uplink_fn, void *);
void uat_stream_feed(uat_stream_t *, const uint8_t *cu8, size_t bytes);
#ifdef __cplusplus
}
#endif

#endif
