/* Sampled hard-bit UAT clock recovery and a seam-preserving SPSC RF ring. */
#ifndef UAT_HARD_H
#define UAT_HARD_H
#include "uat_decode.h"
#include <stddef.h>
#include <stdatomic.h>

#define UAT_HARD_LANES 8
#define UAT_HARD_CHUNK 256
#define UAT_HARD_SLOTS 128
static inline bool uat_hard_rate_valid(uint32_t rate)
{
    return rate == 2000000 || rate == 1500000 || rate == 1250000 || rate == 1041667;
}
typedef struct {
    uint32_t phase;
    uint64_t shift, start;
    unsigned searched, bits;
    bool collecting, up;
    uint8_t frame[48];
} uat_hard_lane_t;
typedef struct {
    uint64_t samples;
    uint32_t sync[2], valid[2], bad, corrected, seams;
    uat_fix_t fix;
    bool have_fix;
} uat_hard_stats_t;
typedef struct {
    uat_hard_lane_t lane[UAT_HARD_LANES];
    uat_hard_stats_t stats;
    uint32_t step;
    unsigned lanes;
    unsigned previous;
    bool have_previous;
    uint64_t sync_at[2], valid_at, bad_at;
    bool have_sync[2], have_valid, have_bad;
} uat_hard_t;
/* Packed MSB-first hard samples. A seam discards partial clock/sync/frame
   state BEFORE these bytes; cumulative counters survive. No allocation. */
void uat_hard_init(uat_hard_t *, uint32_t sample_rate);
void uat_hard_feed(uat_hard_t *, const uint8_t *, size_t, bool seam);

typedef struct {
    uint16_t n;
    bool seam;
    uint8_t bytes[UAT_HARD_CHUNK];
} uat_hard_chunk_t;
typedef struct {
    atomic_uint write, read;
    bool pending_seam; /* producer only */
    uat_hard_chunk_t slot[UAT_HARD_SLOTS];
} uat_hard_ring_t;
/* One producer and one consumer; initialize only with both stopped. A full
   ring drops NEW data and marks the next accepted chunk, never queued data. */
void uat_hard_ring_init(uat_hard_ring_t *);
bool uat_hard_ring_push(uat_hard_ring_t *, const uint8_t *, size_t, bool);
bool uat_hard_ring_pop(uat_hard_ring_t *, uat_hard_chunk_t *);
#endif

