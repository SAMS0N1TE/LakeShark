#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_BTN_DEFAULT = 0,
    LS_BTN_PRIMARY,
    LS_BTN_DANGER,
    LS_BTN_TOGGLE_ON,
    LS_BTN_TOGGLE_OFF
} ls_ui_button_role_t;

typedef enum {
    LS_UI_CONFIRM_PRESS = 0,
    LS_UI_CONFIRM_UPDATE,
    LS_UI_CONFIRM_RELEASE,
    LS_UI_CONFIRM_CANCEL,
    LS_UI_CONFIRM_RESET
} ls_ui_confirm_event_t;

typedef enum {
    LS_UI_CONFIRM_EFFECT_NONE = 0,
    LS_UI_CONFIRM_EFFECT_ARMED,
    LS_UI_CONFIRM_EFFECT_PROGRESS,
    LS_UI_CONFIRM_EFFECT_FIRE,
    LS_UI_CONFIRM_EFFECT_RESET
} ls_ui_confirm_effect_t;

typedef struct {
    uint32_t hold_ms;
    uint32_t started_ms;
    bool active;
    bool fired;
} ls_ui_confirm_state_t;

/* the old hold control kept its transition state inside an LVGL
 * callback, where a short press and duplicate completion could not be tested.
 * This state machine is UI-independent; the shared button below only renders
 * its effects.  Unsigned subtraction intentionally preserves tick wrap. */
void ls_ui_confirm_init(ls_ui_confirm_state_t *state, uint32_t hold_ms);
ls_ui_confirm_effect_t ls_ui_confirm_step(ls_ui_confirm_state_t *state,
                                          ls_ui_confirm_event_t event,
                                          uint32_t now_ms);
uint32_t ls_ui_confirm_remaining_ms(const ls_ui_confirm_state_t *state,
                                    uint32_t now_ms);

/* Longest caption ls_ui_confirm_caption() writes, NUL included: "HOLD 99.9". */
#define LS_UI_CONFIRM_CAPTION_MAX 12u

/* Remaining hold in tenths of a second, truncated, clamped to 99.9 s so a
 * nonsense hold_ms cannot widen the caption past the button. */
uint32_t ls_ui_confirm_remaining_tenths(const ls_ui_confirm_state_t *state,
                                        uint32_t now_ms);

/* Renders that value as the button caption.  Always NUL-terminated, never
 * writes past `cap`; returns the characters written. */
size_t ls_ui_confirm_caption(uint32_t tenths, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
