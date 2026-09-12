#include "ui/ls_safe_screen_hint.h"

#include <stdio.h>
#include <string.h>

/* See the header for the report this came from. Text only; the LVGL
   wrapper in ls_safe_screen.cpp renders whatever this decides. */

/* A duration is shown to one decimal, always - "1.5 s", "2.0 s" - so the
   countdown does not change width as it runs and the line does not reflow.
   Clamped at 99.9 s: nothing sane configures a longer hold, and an absurd
   value must not be allowed to push the line past LS_SAFE_HINT_MAX_CHARS. */
#define HINT_MS_MAX 99900u

static void duration_text(uint32_t ms, char *out, size_t cap)
{
    if (ms > HINT_MS_MAX) ms = HINT_MS_MAX;
    snprintf(out, cap, "%u.%u s", (unsigned)(ms / 1000u),
             (unsigned)((ms % 1000u) / 100u));
}

static uint32_t to_tenths(uint32_t ms)
{
    uint32_t t;
    if (ms > HINT_MS_MAX) ms = HINT_MS_MAX;  /* +99 below must not wrap */
    t = ((ms + 99u) / 100u) * 100u;
    return t ? t : 100u;
}

void ls_safe_hint_init(ls_safe_hint_t *hint, uint32_t hold_ms)
{
    if (!hint) return;
    memset(hint, 0, sizeof(*hint));
    hint->phase = LS_SAFE_HINT_READY;
    hint->hold_ms = hold_ms;
}

static void set_action(ls_safe_hint_t *hint, const char *action)
{
    if (!action) {
        hint->action[0] = '\0';
        return;
    }
    strncpy(hint->action, action, LS_SAFE_HINT_ACTION_MAX - 1u);
    hint->action[LS_SAFE_HINT_ACTION_MAX - 1u] = '\0';
}

bool ls_safe_hint_update(ls_safe_hint_t *hint, ls_ui_confirm_effect_t effect,
                         const char *action, uint32_t remaining_ms)
{
    if (!hint) return false;

    const ls_safe_hint_t before = *hint;

    switch (effect) {
    case LS_UI_CONFIRM_EFFECT_ARMED:
    case LS_UI_CONFIRM_EFFECT_PROGRESS:
        hint->phase = LS_SAFE_HINT_COUNTDOWN;
        set_action(hint, action);
        hint->remaining_ms = to_tenths(remaining_ms);
        break;

    case LS_UI_CONFIRM_EFFECT_FIRE:
        hint->phase = LS_SAFE_HINT_CONFIRMED;
        set_action(hint, action);
        hint->remaining_ms = 0;
        break;

    case LS_UI_CONFIRM_EFFECT_RESET:

        if (hint->phase == LS_SAFE_HINT_COUNTDOWN) {
            hint->phase = LS_SAFE_HINT_CANCELLED;
            hint->remaining_ms = 0;
        }
        break;

    case LS_UI_CONFIRM_EFFECT_NONE:
    default:
        break;
    }

    return before.phase != hint->phase ||
           before.remaining_ms != hint->remaining_ms ||
           strcmp(before.action, hint->action) != 0;
}

size_t ls_safe_hint_format(const ls_safe_hint_t *hint, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!hint) return 0;

    char dur[16];

    switch (hint->phase) {
    case LS_SAFE_HINT_COUNTDOWN:
        duration_text(hint->remaining_ms, dur, sizeof(dur));
        snprintf(out, cap, "%s - KEEP HOLDING %s", hint->action, dur);
        break;

    case LS_SAFE_HINT_CANCELLED:
        duration_text(hint->hold_ms, dur, sizeof(dur));
        snprintf(out, cap, "%s CANCELLED - HOLD %s", hint->action, dur);
        break;

    case LS_SAFE_HINT_CONFIRMED:
        snprintf(out, cap, "%s CONFIRMED", hint->action);
        break;

    case LS_SAFE_HINT_READY:
    default:

        duration_text(hint->hold_ms, dur, sizeof(dur));
        snprintf(out, cap, "HOLD %s TO CONFIRM - A TAP IS IGNORED", dur);
        break;
    }

    return strlen(out);
}

ls_ui_color_role_t ls_safe_hint_color(const ls_safe_hint_t *hint)
{
    if (!hint) return LS_UI_COLOR_DIM_TEXT;
    switch (hint->phase) {
    case LS_SAFE_HINT_COUNTDOWN: return LS_UI_COLOR_WARN;
    case LS_SAFE_HINT_CANCELLED: return LS_UI_COLOR_ALARM;
    case LS_SAFE_HINT_CONFIRMED: return LS_UI_COLOR_ACCENT;
    case LS_SAFE_HINT_READY:
    default:                     return LS_UI_COLOR_DIM_TEXT;
    }
}
