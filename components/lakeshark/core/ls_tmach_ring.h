/* TIME MACHINE, the part with no operating system in it: the staging queue a
   receiver's capture path copies into, the decimating ring the worker keeps
   the last N seconds in, the energy "tape" the screen draws, and freezing a
   window of the ring while it is written out as a clip.

   Who calls what:
     capture task  ls_tm_stage_put / ls_tm_stage_flush   (copy only, never waits)
     worker task   ls_tm_ring_feed, ls_tm_ring_hold*, clip jobs
     UI task       nothing here; it reads snapshots ls_tmach.c publishes

   The stage is single producer, single consumer, lock free. The ring is
   owned by the worker; nothing else touches it.

   Stored formats: a CU8 source (RTL-SDR, HackRF) is kept as CS8, a CS16
   source (LR2021, 125k IQ16) as CS16, each decimated by a whole factor so
   the stored rate is the first at or above keep_hz. The decimator is a
   second order CIC (two integrators, two combs; Hogenauer 1981), unity gain,
   reset at every discontinuity. */
#ifndef LS_TMACH_RING_H
#define LS_TMACH_RING_H

#ifdef __cplusplus
#include <atomic>
#define LS_TM_ATOMIC(t) std::atomic<t>
#else
#include <stdatomic.h>
#define LS_TM_ATOMIC(t) _Atomic t
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sample formats, source and stored. */
#define LS_TM_FMT_NONE 0
#define LS_TM_FMT_CU8  1   /* offset binary 8-bit I,Q: RTL-SDR and HackRF reads */
#define LS_TM_FMT_CS8  2   /* signed 8-bit I,Q                                 */
#define LS_TM_FMT_CS16 3   /* signed 16-bit little endian I,Q: LR2021 blocks    */

/* Radios, as the clip names them. */
#define LS_TM_RADIO_NONE   0
#define LS_TM_RADIO_RTL    1
#define LS_TM_RADIO_HACKRF 2
#define LS_TM_RADIO_LR2021 3
#define LS_TM_RADIO_FILE   4   /* a replayed clip */

static inline unsigned ls_tm_pair_bytes(unsigned fmt)
{
    return fmt == LS_TM_FMT_CS16 ? 4u : fmt ? 2u : 0u;
}
const char *ls_tm_fmt_name(unsigned fmt);        /* "cu8", "cs8", "cs16", "?" */
unsigned    ls_tm_fmt_parse(const char *name);   /* 0 when unknown            */
const char *ls_tm_radio_name(unsigned radio);    /* "RTL-SDR", "HackRF", ...  */
unsigned    ls_tm_radio_parse(const char *name);

/* What a block of samples is. Anything here changing starts a new segment. */
typedef struct {
    uint64_t center_hz;
    uint32_t rate_hz;
    int16_t  gain_tenths;
    uint8_t  fmt;        /* LS_TM_FMT_CU8 or LS_TM_FMT_CS16 */
    uint8_t  radio;      /* LS_TM_RADIO_*                   */
} ls_tm_src_t;

static inline bool ls_tm_src_same(const ls_tm_src_t *a, const ls_tm_src_t *b)
{
    return a->center_hz == b->center_hz && a->rate_hz == b->rate_hz &&
           a->gain_tenths == b->gain_tenths && a->fmt == b->fmt && a->radio == b->radio;
}

/* ------------------------------------------------------------- stage -- */

#define LS_TM_SLOT_BYTES 16384u   /* a multiple of every pair size */
#define LS_TM_SLOTS      16u      /* power of two                  */

typedef struct {
    ls_tm_src_t src;
    int64_t  end_us;      /* when the slot's last byte was tapped           */
    uint32_t bytes;
    bool     gap_before;  /* samples were lost between this and the last one */
} ls_tm_slot_meta_t;

typedef struct {
    LS_TM_ATOMIC(uint32_t) w, r;           /* published / consumed slots */
    LS_TM_ATOMIC(uint32_t) drops;          /* times input was thrown away */
    LS_TM_ATOMIC(uint32_t) dropped_bytes;
    ls_tm_slot_meta_t meta[LS_TM_SLOTS];
    uint8_t *data;                         /* LS_TM_SLOTS * LS_TM_SLOT_BYTES */
    /* Producer only. */
    bool     open, pending_gap;
    uint32_t open_bytes;
    uint64_t stream_bytes;                 /* every byte offered, kept or not */
    ls_tm_src_t open_src;
} ls_tm_stage_t;

void ls_tm_stage_init(ls_tm_stage_t *st, uint8_t *data);
/* Copy `bytes` into the open slot, publishing each slot as it fills. When
   the queue is full the rest is dropped and the next slot says so. Keeps
   whole pairs: after a drop, input is skipped to the next pair boundary of
   the source's byte stream. Returns the bytes kept. */
size_t ls_tm_stage_put(ls_tm_stage_t *st, const ls_tm_src_t *src,
                       const void *data, size_t bytes, int64_t now_us);
/* Publish the part-filled slot, if any. `gap`: what follows is not
   contiguous with what came before (a stop, a retune, a lost block). */
void ls_tm_stage_flush(ls_tm_stage_t *st, bool gap, int64_t now_us);
/* Consumer: the oldest published slot, or NULL; release it when done. */
const ls_tm_slot_meta_t *ls_tm_stage_peek(ls_tm_stage_t *st, const uint8_t **data);
void ls_tm_stage_release(ls_tm_stage_t *st);
/* Consumer: throw everything published away (a source not being recorded). */
unsigned ls_tm_stage_discard(ls_tm_stage_t *st);

/* One stage, several would-be producers (two dongles on the SDR queue, each
   on its own task): the first to arrive owns the stage until it has been
   quiet for stale_us, and a busy flag keeps any two copies from overlapping.
   enter answers whether this caller may write now; every true is paired
   with leave. Zero-initialised is free. */
typedef struct {
    LS_TM_ATOMIC(unsigned) owner;          /* 0 none, else the caller's id */
    LS_TM_ATOMIC(int64_t)  seen_us;        /* the owner's last write       */
    LS_TM_ATOMIC(bool)     busy;
    LS_TM_ATOMIC(uint32_t) collisions;     /* writes refused while busy    */
} ls_tm_claim_t;
bool ls_tm_claim_enter(ls_tm_claim_t *c, unsigned who, int64_t now_us, int64_t stale_us);
void ls_tm_claim_leave(ls_tm_claim_t *c);

/* -------------------------------------------------------------- ring -- */

#define LS_TM_SEGS       32u   /* tuning history kept, data or not */
#define LS_TM_TICKS      600u  /* 60 s of tape at 100 ms           */
#define LS_TM_TICK_US    100000
#define LS_TM_DECIM_MAX  64u

/* One stretch of contiguous samples with one meaning. Kept after its
   samples have been overwritten, as tuning history for the timeline. */
typedef struct {
    ls_tm_src_t src;
    uint64_t first_byte;    /* absolute ring byte of its first stored pair */
    uint64_t pairs;         /* stored pairs written                         */
    int64_t  start_us;      /* time of its first stored pair                */
    int64_t  end_us;        /* time just after its last stored pair         */
    uint32_t rate_hz;       /* stored rate                                  */
    uint32_t id;            /* counts up from 1                             */
    uint16_t decim;
    uint8_t  fmt;           /* stored format                                */
    uint8_t  pair_bytes;
} ls_tm_seg_t;

/* One 100 ms column of the tape. level is mean power, 0.5 dB steps above
   -100 dBFS (0 = silent or no data); flags mark what happened in it. */
#define LS_TM_TICK_DATA  1u
#define LS_TM_TICK_SEG   2u   /* a segment (tune, rate, radio) started here */
#define LS_TM_TICK_GAP   4u   /* samples were lost                          */
#define LS_TM_TICK_HELD  8u   /* the writer was held back by a clip save    */
typedef struct {
    int64_t end_us;
    uint8_t level, peak, flags, seg;   /* seg: low byte of the segment id */
} ls_tm_tick_t;

typedef struct {
    uint8_t *buf;
    size_t   cap;          /* bytes, multiple of 4 */
    uint64_t head;         /* absolute bytes written */
    uint32_t keep_hz;      /* the narrowest rate kept */

    ls_tm_seg_t seg[LS_TM_SEGS];
    uint32_t seg_count;    /* segments ever started; newest is seg_count-1 */
    bool     need_seg;     /* the next pair starts a new segment            */

    /* Clip saving: bytes at or after `hold` may not be overwritten. */
    bool     held;
    uint64_t hold;
    uint32_t held_drops;   /* blocks refused because of the hold */
    uint32_t gaps;

    /* CIC2 state, per I and Q. Unsigned so wraparound is defined. */
    uint32_t s1[2], s2[2], z1[2], z2[2];
    uint32_t phase;
    bool     cic_primed;

    /* Tape. */
    ls_tm_tick_t tick[LS_TM_TICKS];
    uint32_t tick_count;           /* ticks closed, ever */
    double   tick_power;           /* sum of |x|^2, normalised to full scale */
    double   tick_peak;
    uint32_t tick_pairs, tick_need;
    uint8_t  tick_flags;
    int64_t  tick_end_us;
} ls_tm_ring_t;

/* `buf` must outlive the ring; cap is rounded down to a multiple of 4. */
void ls_tm_ring_init(ls_tm_ring_t *r, uint8_t *buf, size_t cap, uint32_t keep_hz);
/* Forget all samples and history, keep the buffer. */
void ls_tm_ring_reset(ls_tm_ring_t *r);
/* The factor a source of `rate_hz` is decimated by. */
unsigned ls_tm_decim_for(uint32_t rate_hz, uint32_t keep_hz);

/* Worker: one staged block. Decimates, stores, updates the tape. Returns
   the stored pairs written. */
size_t ls_tm_ring_feed(ls_tm_ring_t *r, const ls_tm_slot_meta_t *meta,
                       const uint8_t *data);
/* A discontinuity with no data (a stop): the next block starts a segment. */
void ls_tm_ring_break(ls_tm_ring_t *r);

/* Oldest absolute byte still in the ring. */
static inline uint64_t ls_tm_ring_floor(const ls_tm_ring_t *r)
{
    return r->head > r->cap ? r->head - r->cap : 0;
}
const ls_tm_seg_t *ls_tm_ring_seg_newest(const ls_tm_ring_t *r);
/* The segment whose time span holds `t_us`, data or not; NULL if none. */
const ls_tm_seg_t *ls_tm_ring_seg_at(const ls_tm_ring_t *r, int64_t t_us);
/* Seconds of the newest segment still held in the ring. */
double ls_tm_ring_held_s(const ls_tm_ring_t *r);
/* Copy `bytes` from absolute offset `at` (must still be in the ring). */
bool ls_tm_ring_read(const ls_tm_ring_t *r, uint64_t at, void *out, size_t bytes);

/* ----------------------------------------------------- clip windows -- */

/* A window of one segment, frozen while it is copied out. */
typedef struct {
    ls_tm_seg_t seg;        /* the segment, as it was when the job began */
    uint64_t from, to;      /* absolute bytes [from, to)                 */
    uint64_t at;            /* next byte to copy                         */
    int64_t  start_us, end_us;
    bool     active;
} ls_tm_window_t;

/* Freeze [t_from, t_to] of the segment holding t_to (or the newest one when
   t_to is 0), clamped to what is still in the ring. False when there is
   nothing to freeze. Holds the ring at the window start. */
bool ls_tm_window_begin(ls_tm_ring_t *r, ls_tm_window_t *w, int64_t t_from_us, int64_t t_to_us);
/* Copy up to `cap` bytes of the window, advancing it and the hold. Returns
   the bytes copied; 0 at the end. */
size_t ls_tm_window_step(ls_tm_ring_t *r, ls_tm_window_t *w, void *out, size_t cap);
/* Release the hold, whether or not the window was finished. */
void ls_tm_window_end(ls_tm_ring_t *r, ls_tm_window_t *w);

#ifdef __cplusplus
}
#endif

#endif
