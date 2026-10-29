/* TIME MACHINE: while any receiver runs, the board keeps the last N seconds
   of its I/Q, so "WHAT WAS THAT?" can save the moment you just missed, and
   a clip can be played back through the decoders afterwards.

   Sources: every RTL-SDR / HackRF read through the radio endpoint
   (ls_radio_endpoint_set_capture_tap) and every LR2021 block lr_wf_session hands
   its consumer, narrow or wide (lr_wf_set_capture_tap). The taps copy into a lock
   free stage and return; a worker task (core 0, low priority) decimates
   into the PSRAM ring, keeps the tape and the timeline, and writes clips.
   Off, the taps are one atomic load.

   Memory: the ring (budget_kb, PSRAM, from the first sample after it is
   switched on) plus 2 x 256 KB of stage; freed when switched off, and when
   no receiver has tapped for 60 s (back on the next tap).

   Settings live in /sdcard/lakeshark/tmach/config.json (version 1, written
   by the worker; data/tmach_config.json documents the defaults). No NVS.

   The screen calls only the snapshot and request functions below; none of
   them waits. */
#ifndef LS_TMACH_H
#define LS_TMACH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ls_tmach_clip.h"
#include "ls_tmach_ring.h"
#include "ls_tmach_timeline.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_TM_Q_SDR 0
#define LS_TM_Q_LR  1
#define LS_TM_QUEUES 2

#define LS_TM_SOURCE_AUTO 0   /* whichever receiver started last */
#define LS_TM_SOURCE_SDR  1
#define LS_TM_SOURCE_LR   2

#define LS_TM_CONFIG_VERSION 1
typedef struct {
    bool     enabled;
    uint16_t budget_kb;    /* ring size, 1024 .. 16384                    */
    uint32_t keep_hz;      /* narrowest rate kept, 25000 .. 1000000       */
    uint8_t  source;       /* LS_TM_SOURCE_*                              */
    bool     autoclip;     /* save a clip around every decoded event      */
    uint16_t clip_s;       /* what WHAT WAS THAT saves, 5 .. 120 s        */
    uint32_t max_rate;     /* a faster source is not recorded (ADS-B)     */
} ls_tm_config_t;

void ls_tm_config_defaults(ls_tm_config_t *c);
/* Clamp to the ranges above; false if anything had to change. */
bool ls_tm_config_valid(ls_tm_config_t *c);
int  ls_tm_config_format(char *out, size_t n, const ls_tm_config_t *c);
bool ls_tm_config_parse(const char *in, ls_tm_config_t *c);
void ls_tm_config_get(ls_tm_config_t *c);
/* Takes effect on the worker's next pass and is saved to the card. */
void ls_tm_config_set(const ls_tm_config_t *c);

typedef struct {
    bool     enabled, allocated, recording;   /* recording: samples in the last second */
    char     why[64];                         /* not recording: why, in words */
    ls_tm_seg_t seg;                          /* the newest segment (seg.id 0: none) */
    double   held_s, capacity_s;
    uint32_t ring_kb;
    uint32_t drops, gaps, held_drops, too_fast;
    uint32_t tick_count;
    /* WHAT WAS THAT */
    bool     saving;
    uint8_t  save_pct;
    int64_t  save_from_us, save_to_us;        /* the window being written */
    uint32_t saving_no, last_clip, clips_saved;
    double   last_clip_s;
    char     note[64];                        /* the last thing that happened */
    /* replay */
    bool     replaying;
    uint8_t  replay_pct;
    uint32_t replay_no, replay_found;
    /* timeline */
    uint32_t tl_version, tl_count;
} ls_tm_status_t;

/* Board: taps, worker. Safe to call once at boot. */
void ls_tm_init(void);
/* One pass of the worker; the board's task loops it, the bench calls it. */
void ls_tm_service(int64_t now_us);

/* Capture side (any task). Copy only; never waits. */
void ls_tm_tap(int queue, const ls_tm_src_t *src, const void *data, size_t bytes, int64_t now_us);
void ls_tm_tap_end(int queue, int64_t now_us);

/* Screen side: snapshots, false / 0 when the worker holds them right now
   (keep the last one). Tape ticks oldest first, up to max, the newest last. */
uint32_t ls_tm_tape(ls_tm_tick_t *out, uint32_t max, uint32_t *tick_count);
/* Timeline rows newest first from `first`. *total gets the row count. */
uint32_t ls_tm_events(ls_tm_event_t *out, uint32_t first, uint32_t max, uint32_t *total);
bool     ls_tm_status(ls_tm_status_t *out);

/* Requests, done by the worker. */
bool ls_tm_what_was_that(void);              /* false: off, nothing held, or busy */
bool ls_tm_replay(uint32_t clip_no);         /* 0: the newest clip */

/* Playing a clip into decoders. The screen's component registers the
   decoders it has (POCSAG on FM's chain and on the LR pager chain); the
   worker opens, feeds and closes them while it steps through the clip. */
typedef struct {
    /* false and why when nothing here decodes this clip's rate and format */
    bool (*open)(const ls_tm_clip_meta_t *m, char *why, size_t n);
    /* source-rate samples; at_us is the board time of the first of them */
    void (*feed)(const uint8_t *iq, size_t bytes, int64_t at_us);
    /* how many things were decoded; a line saying what */
    uint32_t (*close)(char *note, size_t n);
} ls_tm_decoder_t;
void ls_tm_set_decoder(const ls_tm_decoder_t *d);
/* What the receiver on `radio` (LS_TM_RADIO_*) is decoding, for a clip. */
void ls_tm_set_decoder_name(const char *(*fn)(unsigned radio));

/* Console: `tm` (status | on | off | what | replay [n] | clips | events). */
int ls_tm_command(int argc, char **argv);
void ls_tm_console_register(void);        /* board only */
/* Where clips go; LS_TM_CLIP_DIR unless a bench says otherwise. */
void ls_tm_set_clip_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif
