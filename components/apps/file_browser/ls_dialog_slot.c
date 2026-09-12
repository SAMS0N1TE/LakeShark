/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ls_dialog_slot.h"

#include <stddef.h>

/**/
/* Intentionally free of LVGL, IDF and audio_player headers so the bench can
   link this file straight in and exercise pause/close ordering under mocks.
   The comment in the header explains what this fixes. */

static ls_dialog_slot_hooks_t s_hooks;

void ls_dialog_slot_configure(const ls_dialog_slot_hooks_t *hooks)
{
    if (hooks) {
        s_hooks = *hooks;
    } else {
        s_hooks.del      = NULL;
        s_hooks.is_valid = NULL;
    }
}

static bool alive(void *obj)
{
    if (!obj) return false;
    if (!s_hooks.is_valid) return true;
    return s_hooks.is_valid(obj);
}

void ls_dialog_slot_forget(ls_dialog_slot_t *slot)
{
    if (slot) slot->obj = NULL;
}

void ls_dialog_slot_set(ls_dialog_slot_t *slot, void *obj)
{
    if (slot) slot->obj = obj;
}

void ls_dialog_slot_close(ls_dialog_slot_t *slot)
{
    if (!slot) return;
    void *obj = slot->obj;

    slot->obj = NULL;
    if (obj && alive(obj) && s_hooks.del) {
        s_hooks.del(obj);
    }
}

bool ls_dialog_slot_is_open(const ls_dialog_slot_t *slot)
{
    if (!slot) return false;
    if (!slot->obj) return false;
    if (!s_hooks.is_valid) return true;
    return s_hooks.is_valid(slot->obj);
}
