/* TIME MACHINE's event timeline: everything the decoders and radios
   reported, one list in time order, each entry with its frequency and radio
   and the clip it sits in when one was saved around it.

   It is a reader of ls_decoded (core/ls_decoded.h), the board's one decoded
   bus - not a second bus. The worker drains its reader into here; producers
   that have no ls_decoded kind yet publish one (P25, REC, DETECT: see the
   kinds appended there).

   Ordering is by the decoder's own at_us, so an entry that arrives late
   lands where it happened. Repeats merge: the same aircraft, vessel, page,
   call or detection inside its kind's window bumps a count instead of
   adding a row. Pure C; the worker owns it, the screen reads copies. */
#ifndef LS_TMACH_TIMELINE_H
#define LS_TMACH_TIMELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ls_decoded.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_TM_TL_MAX    128
#define LS_TM_TL_CLIPS  16
#define LS_TM_TL_TEXT   96

typedef struct {
    int64_t  at_us, last_us;      /* first and latest report */
    double   lat, lon;
    uint32_t freq_hz;
    uint32_t address;
    uint32_t clip;                /* clip number holding at_us, 0 none */
    int32_t  clip_ms;             /* where at_us falls in it            */
    uint32_t seq;                 /* unique, counts up                  */
    uint16_t kind;                /* LS_DECODED_*                       */
    uint16_t count;               /* reports merged into this row       */
    uint8_t  radio;               /* LS_TM_RADIO_*                      */
    bool     has_pos;
    char     from[24];
    char     text[LS_TM_TL_TEXT];
} ls_tm_event_t;

/* What was tuned at a moment, for an entry that does not say. */
typedef bool (*ls_tm_tuned_fn)(void *ctx, int64_t at_us, uint32_t *freq_hz, uint8_t *radio);

typedef struct {
    ls_tm_event_t ev[LS_TM_TL_MAX];      /* oldest first */
    uint32_t n;
    uint32_t added, merged, dropped;     /* dropped: oldest pushed out */
    uint32_t next_seq;
    uint32_t version;                    /* changes with any edit      */
    struct { uint32_t no; int64_t from_us, to_us; } clip[LS_TM_TL_CLIPS];
    uint32_t clips;
} ls_tm_timeline_t;

void ls_tm_tl_init(ls_tm_timeline_t *tl);
/* Merge window for a kind, in microseconds; 0 never merges. */
int64_t ls_tm_tl_window(uint16_t kind);
/* Add or merge one decoded entry. Returns the row's seq. */
uint32_t ls_tm_tl_add(ls_tm_timeline_t *tl, const ls_decoded_t *d,
                      ls_tm_tuned_fn tuned, void *ctx);
/* A clip now covers [from_us, to_us]: link the rows inside it, and rows that
   arrive later and fall inside it. */
void ls_tm_tl_clip(ls_tm_timeline_t *tl, uint32_t no, int64_t from_us, int64_t to_us);
/* Rows, newest first: index 0 is the latest. */
const ls_tm_event_t *ls_tm_tl_newest(const ls_tm_timeline_t *tl, uint32_t index);
/* One letter and a word for a kind: 'P' "PAGE". */
char        ls_tm_kind_letter(uint16_t kind);
const char *ls_tm_kind_name(uint16_t kind);

#ifdef __cplusplus
}
#endif

#endif
