#ifndef LS_RADIO_HOLDERS_H
#define LS_RADIO_HOLDERS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* One lease per task: polling a pending request does not add another hold.
   The first holder owns the part; later holders wait without disturbing it.
   Background holders rank by claim instead (ls_radio_claims_t below). */
typedef struct { uintptr_t task[8]; unsigned count; } ls_radio_holders_t;
static inline bool ls_radio_holders_set(ls_radio_holders_t *h, uintptr_t task, bool on)
{
    if (!task) return false;
    unsigned i = 0;
    while (i < h->count && h->task[i] != task) i++;
    if (on) {
        if (i == h->count) {
            if (h->count == 8) return false;
            h->task[h->count++] = task;
        }
    } else if (i < h->count) {
        memmove(h->task + i, h->task + i + 1,
                (h->count - i - 1) * sizeof(h->task[0]));
        h->count--;
    }
    return true;
}
static inline bool ls_radio_holders_owns(const ls_radio_holders_t *h, uintptr_t task)
{
    return h->count && h->task[0] == task;
}

/* Background claims; the newest outranks the rest. A task keeps its serial
   while it keeps asking; silent for the quiet time, its claim lapses and its
   next ask is a new, newer one. A holder dropped for its silence keeps a
   claim ranked below every other until it releases, so it is told to let go
   and cannot take the chip back from whoever has it now. */
typedef struct {
    uintptr_t task[8];
    uint32_t  serial[8];
    int64_t   seen[8];
    bool      dropped[8];
    uint32_t  next, low;
} ls_radio_claims_t;
#define LS_RADIO_CLAIM_BASE 0x80000000u
static inline unsigned ls_radio_claims_find(const ls_radio_claims_t *c, uintptr_t task)
{
    unsigned i = 0;
    while (i < 8 && (!task || c->task[i] != task)) i++;
    return i;
}
/* The task's serial, or 0 when it has no live claim. */
static inline uint32_t ls_radio_claims_peek(const ls_radio_claims_t *c, uintptr_t task,
                                            int64_t now, int64_t quiet)
{
    const unsigned i = ls_radio_claims_find(c, task);
    return i == 8 || (!c->dropped[i] && now - c->seen[i] > quiet) ? 0 : c->serial[i];
}
/* Dropped and not yet released. The mark outlives any stall a live holder
   could have, not a task that ended: after `life` it is forgotten. */
static inline bool ls_radio_claims_dropped(ls_radio_claims_t *c, uintptr_t task,
                                           int64_t now, int64_t life)
{
    const unsigned i = ls_radio_claims_find(c, task);
    if (i == 8 || !c->dropped[i]) return false;
    if (now - c->seen[i] <= life) return true;
    c->task[i] = 0; c->dropped[i] = false;
    return false;
}
/* The task asked. A live claim is kept; a lapsed or missing one is made
   again only with `claim`. Returns its serial, or 0 with no live claim. */
static inline uint32_t ls_radio_claims_touch(ls_radio_claims_t *c, uintptr_t task,
                                             int64_t now, int64_t quiet, bool claim)
{
    if (!task) return 0;
    unsigned i = ls_radio_claims_find(c, task), slot = 0;
    if (i < 8 && c->dropped[i]) return c->serial[i];
    if (i < 8 && now - c->seen[i] <= quiet) { c->seen[i] = now; return c->serial[i]; }
    if (!claim) return 0;
    /* A free slot, else the stalest, a dropped claim last of all. */
    for (unsigned k = 1; i == 8 && k < 8 && c->task[slot]; k++)
        if (!c->task[k] || (c->dropped[slot] && !c->dropped[k]) ||
            (c->dropped[slot] == c->dropped[k] && c->seen[k] < c->seen[slot])) slot = k;
    if (i < 8) slot = i;
    if (c->next < LS_RADIO_CLAIM_BASE) c->next = LS_RADIO_CLAIM_BASE;
    c->task[slot] = task; c->serial[slot] = ++c->next; c->seen[slot] = now;
    c->dropped[slot] = false;
    return c->next;
}
/* Its holder was dropped while it may still be on the part. */
static inline void ls_radio_claims_drop(ls_radio_claims_t *c, uintptr_t task, int64_t now)
{
    const unsigned i = ls_radio_claims_find(c, task);
    if (i == 8) return;
    if (!c->low || c->low > LS_RADIO_CLAIM_BASE) c->low = LS_RADIO_CLAIM_BASE;
    c->dropped[i] = true; c->serial[i] = --c->low; c->seen[i] = now;
}
/* The task released. Giving way to a holder that outranks it, it keeps its
   place behind that holder; otherwise the claim ends here. */
static inline void ls_radio_claims_release(ls_radio_claims_t *c, uintptr_t task,
                                           int64_t now, bool outranked)
{
    const unsigned i = ls_radio_claims_find(c, task);
    if (i == 8) return;
    c->dropped[i] = false;
    if (outranked) c->seen[i] = now;
    else c->task[i] = 0;
}
#endif
