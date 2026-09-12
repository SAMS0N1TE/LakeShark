

#ifndef LS_VITALS_H
#define LS_VITALS_H

#include <stdint.h>

#include "ls_trend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Two seconds. A leak shows over minutes and a mode change over seconds, and
   this is the interval at which a screen-width of trace covers both. */
#define LS_VITALS_INTERVAL_US 2000000

/* Take a sample if one is due. Cheap on every other call - a compare
   against the last stamp - so the frame loop can call it unconditionally.

   Safe to call before the heaps are interesting and safe to call from one
   task only: this keeps plain statics, and the caller is the shell's own
   loop. */
void ls_vitals_tick(int64_t now_us);

/* Free space over time. Never NULL.

   In whatever unit suits the quantity - see the note in ls_vitals.c, which
   records what happened when they all shared one. Nothing should read these
   as a quantity: a trace scales to its own window, and the CURRENT figure
   comes from the allocator directly. These say which way it has been going,
   and that is all they are for. */
const ls_trend_t *ls_vitals_internal(void);
const ls_trend_t *ls_vitals_largest(void);
const ls_trend_t *ls_vitals_psram(void);

const ls_trend_t *ls_vitals_dma(void);
uint32_t ls_vitals_dma_low(void);

/* The allocator's own all-time low since boot, in bytes. Zero if unknown. */
uint32_t ls_vitals_internal_low(void);
uint32_t ls_vitals_psram_low(void);

/* How many seconds of samples have been taken. Not the same as uptime, and
   saying which is the point - see the header note. */
uint32_t ls_vitals_watched_s(void);

/* WHERE THE MEMORY WENT DURING BOOT. */

#define LS_VITALS_MARKS 12

typedef struct {
    char     name[14];
    uint32_t internal;   /* free bytes */
    uint32_t dma;
    uint32_t psram_kb;
} ls_vitals_mark_t;

/* Record one. Ignores a name it has already seen and does nothing once full,
   so a boot that loops cannot push the early stages out of the record. */
void ls_vitals_mark(const char *name);

int  ls_vitals_mark_count(void);
bool ls_vitals_mark_at(int i, ls_vitals_mark_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LS_VITALS_H */
