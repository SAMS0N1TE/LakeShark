/* What WARDRIVE hears and where it writes: the board's scanners, GPS and card
   (wd_feed_board.c), or the simulator's drive (bench/tools/lssim_wardrive.c).
   Only the worker calls the poll functions. */
#ifndef WD_FEED_H
#define WD_FEED_H

#include "wd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { WD_PACE_FAST, WD_PACE_NORMAL, WD_PACE_SAVER };

/* Turn the scanners on for a session, and off again. */
void wd_feed_start(const bool src[WD_KINDS], int pace);
void wd_feed_stop(void);
void wd_feed_pace(int pace);
/* Whether the board has the source at all (CELL: an LTE-capable build with a receiver). */
bool wd_feed_has(int kind);
/* The position now. */
bool wd_feed_fix(wd_fix_t *out, uint32_t now_ms);
/* What was heard since the last call; returns how many. *scans goes up by
   one for each Wi-Fi scan that finished. */
int  wd_feed_poll(wd_obs_t *out, int cap, uint32_t now_ms, unsigned *scans);

/* The worker: a task on the board that runs wd_step until the session ends
   and its exports are written. False where there is none (the simulator):
   the screen then steps the session from its frame. */
bool wd_feed_spawn(void);
bool wd_feed_worker_alive(void);
void wd_lock(void);
void wd_unlock(void);
void *wd_feed_alloc(size_t bytes);  /* zeroed, PSRAM on the board */
void wd_feed_free(void *p);

/* The card. */
bool wd_feed_card(void);
const char *wd_feed_dir(void);      /* made if missing; "" when it cannot be */
const char *wd_feed_board(void);
/* The hub's locker (ls_hub.h): queue an upload; what the hub says now. */
bool wd_feed_upload(const char *path);
bool wd_feed_hub(char *out, size_t cap, bool *busy);

#ifdef __cplusplus
}
#endif
#endif
