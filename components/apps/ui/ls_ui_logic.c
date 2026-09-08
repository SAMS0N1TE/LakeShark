#include "ui/ls_ui_logic.h"

#include <stdio.h>
#include <string.h>

#define LS_UI_CONFIRM_DEFAULT_MS 2000U

/* LS-1011: 99.9 s.  hold_ms is caller-supplied and an absurd one must clamp
 * rather than render a caption wider than the button that carries it. */
#define LS_UI_CONFIRM_TENTHS_MAX 999U

static void confirm_clear(ls_ui_confirm_state_t *state)
{
    state->started_ms = 0;
    state->active = false;
    state->fired = false;
}

void ls_ui_confirm_init(ls_ui_confirm_state_t *state, uint32_t hold_ms)
{
    if (!state) return;
    state->hold_ms = hold_ms ? hold_ms : LS_UI_CONFIRM_DEFAULT_MS;
    confirm_clear(state);
}

ls_ui_confirm_effect_t ls_ui_confirm_step(ls_ui_confirm_state_t *state,
                                          ls_ui_confirm_event_t event,
                                          uint32_t now_ms)
{
    if (!state) return LS_UI_CONFIRM_EFFECT_NONE;

    switch (event) {
    case LS_UI_CONFIRM_PRESS:
        if (state->active || state->fired)
            return LS_UI_CONFIRM_EFFECT_NONE;
        state->started_ms = now_ms;
        state->active = true;
        return LS_UI_CONFIRM_EFFECT_ARMED;

    case LS_UI_CONFIRM_UPDATE:
        if (!state->active || state->fired)
            return LS_UI_CONFIRM_EFFECT_NONE;
        if ((uint32_t)(now_ms - state->started_ms) >= state->hold_ms) {
            state->fired = true;
            return LS_UI_CONFIRM_EFFECT_FIRE;
        }
        return LS_UI_CONFIRM_EFFECT_PROGRESS;

    case LS_UI_CONFIRM_RELEASE:
    case LS_UI_CONFIRM_CANCEL:
        if (!state->active && !state->fired)
            return LS_UI_CONFIRM_EFFECT_NONE;
        confirm_clear(state);
        return LS_UI_CONFIRM_EFFECT_RESET;

    case LS_UI_CONFIRM_RESET:
        confirm_clear(state);
        return LS_UI_CONFIRM_EFFECT_RESET;

    default:
        return LS_UI_CONFIRM_EFFECT_NONE;
    }
}

uint32_t ls_ui_confirm_remaining_ms(const ls_ui_confirm_state_t *state,
                                    uint32_t now_ms)
{
    if (!state || !state->active || state->fired) return 0;
    const uint32_t elapsed = (uint32_t)(now_ms - state->started_ms);
    return elapsed >= state->hold_ms ? 0 : state->hold_ms - elapsed;
}

uint32_t ls_ui_confirm_remaining_tenths(const ls_ui_confirm_state_t *state,
                                        uint32_t now_ms)
{
    /* LS-1011: truncating division, not (ms + 99) / 100.  Rounding up is what
     * made a 1500 ms hold read "HOLD 2" at the press; the caption must never
     * claim more time is left than really is. */
    const uint32_t tenths = ls_ui_confirm_remaining_ms(state, now_ms) / 100U;
    return tenths > LS_UI_CONFIRM_TENTHS_MAX ? LS_UI_CONFIRM_TENTHS_MAX : tenths;
}

size_t ls_ui_confirm_caption(uint32_t tenths, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (tenths > LS_UI_CONFIRM_TENTHS_MAX) tenths = LS_UI_CONFIRM_TENTHS_MAX;
    snprintf(out, cap, "HOLD %u.%u", (unsigned)(tenths / 10U),
             (unsigned)(tenths % 10U));
    return strlen(out);
}
