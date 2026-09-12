/* Quick controls: the things you reach for without leaving the screen. */

#ifndef LS_QUICK_H
#define LS_QUICK_H

#include <stdbool.h>

#include "ls_action.h"
#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_QUICK_ACTION = 0,
    LS_QUICK_STEP,
    LS_QUICK_TOGGLE,
    LS_QUICK_CYCLE,
} ls_quick_kind_t;

typedef struct {
    const char     *label;    /* what the button says, kept short         */
    ls_quick_kind_t kind;
    const char     *action;   /* ls_action path this calls                */
    const char     *value;    /* ls_value path it reads first, or NULL    */

    /* STEP */
    float           delta;    /* added to the value that was read         */
    float           lo, hi;   /* clamped into this, inclusive             */

    /* CYCLE: the argument walks this list, wrapping. Numbers are given as
       text and parsed by the action's own signature, so a cycle over an int
       action and one over a text action are described the same way. */
    const char *const *choices;
    int             nchoices;

    /* Two keys, because a setting is one control with two
       directions. `key` steps up or forward, `key_down` steps down or back.
       The first version made each direction its own control, which put the
       same value under both of them and invited the reader to look for a
       difference that was not there. */
    char            key;      /* up or forward, 0 for none                */
    char            key_down; /* down or back, 0 for none                 */

    /* Set by the panel when the back target was the one pressed. A caller
       building a table leaves it alone. */
    bool            cycle_back;
} ls_quick_t;

/* Draw the panel into `area`: one row per control. */

/* How many rows these controls need, in a panel this wide. */

int ls_quick_rows(const ls_quick_t *items, int n, int width, bool wide);

int ls_quick_draw_posture(tui_surface *sf, tui_rect area, bool wide,
                  const ls_quick_t *items, int n);

/* Portrait, which is what every caller wanted before the posture existed. */
#define ls_quick_draw(sf, area, items, n)     ls_quick_draw_posture((sf), (area), false, (items), (n))

/* Route a tap. True when it landed on a control, whether or not the action
   the control names succeeded: the tap was still consumed and must not fall
   through to whatever is underneath. `out_status` reports what the action
   said, so a screen can show it; pass NULL to ignore it. */
bool ls_quick_touch(int col, int row, const ls_quick_t *items, int n,
                    ls_cap_t granted, ls_act_status_t *out_status);

/* Route a key against the controls' shortcuts. Same contract as the tap. */
bool ls_quick_key(char ch, const ls_quick_t *items, int n,
                  ls_cap_t granted, ls_act_status_t *out_status);

/* Run one control, which is what both routes above end up doing. */

ls_act_status_t ls_quick_fire(const ls_quick_t *item, ls_cap_t granted);

/* What a built-in screen's panel is granted: everything but transmit. The
   broker gates emission separately, and no quick control is worth the risk
   of a thumb finding it. */
ls_cap_t ls_quick_grant_builtin(void);

/* What the control would show right now: the value it reads, formatted, or
   NULL when it reads nothing. The returned pointer is a static buffer that
   the next call overwrites. */
const char *ls_quick_state(const ls_quick_t *item);

#ifdef __cplusplus
}
#endif

#endif /* LS_QUICK_H */
