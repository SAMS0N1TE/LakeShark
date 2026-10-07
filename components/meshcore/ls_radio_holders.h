#ifndef LS_RADIO_HOLDERS_H
#define LS_RADIO_HOLDERS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* One lease per task: polling a pending request does not add another hold.
   The first holder owns the part; later holders wait without disturbing it. */
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
#endif
