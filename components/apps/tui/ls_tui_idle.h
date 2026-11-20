/* Idle cadence for the UI loop.

   A screen that has not changed for LS_IDLE_AFTER frames in a row, with no
   input, is drawn at a slower period until a frame changes or input wakes
   the loop. The draw still runs every tick, so nothing a screen reads is
   missed; only the rate drops. Pure logic, no hardware, tested on the host. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LS_IDLE_AFTER   20     /* identical frames before slowing (about 0.8 s at 40 ms) */
#define LS_IDLE_SETTLE  3      /* fast frames after one isolated change, then slow again */
#define LS_IDLE_SLOW_MS 100

typedef struct {
    uint32_t streak;           /* identical frames in a row */
    uint32_t frames, clean;    /* frames seen, and how many changed nothing */
    uint32_t slow, wakes;      /* frames paced slow, input wakes out of slow */
    uint32_t breaks, best;     /* changed frames that were not busy; longest identical run seen */
} ls_idle_t;

/* After a frame: `clean` = the present drew and sent nothing, `busy` = a
   reason to keep full rate (input held, remote viewer, recorder, a source
   streaming). Returns the period to pace this frame at, in ms. */
static inline int ls_idle_period(ls_idle_t *g, bool clean, bool busy, int base_ms)
{
    g->frames++;
    if (clean) g->clean++;
    /* One changed frame out of a slow run (a clock's seconds) costs only a
       few fast frames; a second change straight after, or anything busy, is
       full rate again. */
    if (busy) g->streak = 0;
    else if (!clean) { g->breaks++; g->streak = g->streak >= LS_IDLE_AFTER ? LS_IDLE_AFTER - LS_IDLE_SETTLE : 0; }
    else if (g->streak < UINT32_MAX) g->streak++;
    if (g->streak > g->best) g->best = g->streak;
    if (g->streak >= LS_IDLE_AFTER && base_ms < LS_IDLE_SLOW_MS) {
        g->slow++;
        return LS_IDLE_SLOW_MS;
    }
    return base_ms;
}

/* Input (a key, a tap, a touch edge) ended the sleep: full rate at once. */
static inline void ls_idle_wake(ls_idle_t *g)
{
    if (g->streak >= LS_IDLE_AFTER) g->wakes++;
    g->streak = 0;
}
