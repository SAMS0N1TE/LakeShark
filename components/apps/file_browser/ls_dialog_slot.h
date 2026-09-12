/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**/
/* Lifetime slot for a single top-level LVGL object owned by an app.  The
   module deliberately does not include LVGL: it operates on opaque object
   handles through configurable hooks so the bench can drive the same
   open/close ordering under mocks that catch the exact defect this fixes -
   an app pausing or closing while its modal dialog is still parented to the
   screen root, leaving a live overlay on top of whatever the shell launches
   next. */

typedef struct {
    void *obj;
} ls_dialog_slot_t;

typedef struct {
    void (*del)(void *obj);              /* called to destroy a live handle   */
    bool (*is_valid)(const void *obj);   /* NULL means "any non-NULL is live" */
} ls_dialog_slot_hooks_t;

/* Install the hooks the module will use for del/is_valid.  Passing NULL clears
   them and is what the bench uses between cases. */
void ls_dialog_slot_configure(const ls_dialog_slot_hooks_t *hooks);

/* Zero the slot without deleting whatever it held.  Only useful in tests that
   want to assert "an old pointer was left dangling" - production code should
   use ls_dialog_slot_close. */
void ls_dialog_slot_forget(ls_dialog_slot_t *slot);

/* Record a newly created object in the slot.  Does NOT delete whatever was
   there before; callers must call ls_dialog_slot_close first if they want the
   previous object destroyed. */
void ls_dialog_slot_set(ls_dialog_slot_t *slot, void *obj);

/* If the slot holds a live object, invoke the del hook on it.  Always clears
   the slot afterwards, so a stale pointer cannot be handed to a click
   callback that fires after the container went away.  Safe on empty slots and
   safe to call from pause(), close() and back() in any order. */
void ls_dialog_slot_close(ls_dialog_slot_t *slot);

/* True iff the slot holds a non-NULL handle the is_valid hook agrees is
   still alive.  A NULL is_valid hook means "any non-NULL handle is live",
   which is what production LVGL wiring uses. */
bool ls_dialog_slot_is_open(const ls_dialog_slot_t *slot);

/* Raw handle for callers that need to hand it back to their UI library.  May
   be NULL or stale - always check ls_dialog_slot_is_open first. */
static inline void *ls_dialog_slot_obj(const ls_dialog_slot_t *slot)
{
    return slot ? slot->obj : (void *)0;
}

#ifdef __cplusplus
}
#endif
