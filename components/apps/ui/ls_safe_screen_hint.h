#pragma once

/* LS-1010  The recovery screen's actions are hold-to-confirm and nothing on
   the screen said so before the operator touched one.

   Reported from the 4.3 LCD board: tapping TRY NORMAL BOOT "appears to do
   nothing"; holding it rebooted. That is exactly what the code does - a tap
   is LS_UI_CONFIRM_PRESS followed by LS_UI_CONFIRM_RELEASE well short of
   LS_SAFE_SCREEN_HOLD_MS, so the confirmation resets and the shared hold
   button quietly restores its own caption. On a screen whose entire purpose
   is to get a bricked board moving again, a control that silently refuses is
   indistinguishable from a control that is dead.

   The hold stays. What was missing is the sentence that explains it, said
   before the first press and kept adjacent to the buttons rather than at the
   end of a scrollable report nobody reads while poking at a button.

   This module is the text of that affordance and nothing else. It is
   deliberately free of LVGL so the host gate can pin what the operator
   actually reads in each phase - including the one that is easy to get
   wrong, where the release that follows a COMPLETED hold arrives as the same
   LS_UI_CONFIRM_EFFECT_RESET as an abandoned one and must not repaint
   "CONFIRMED" as "CANCELLED". */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/ls_ui_logic.h"
#include "ui/ls_ui_palette.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest action caption the hint will quote back. "TRY NORMAL BOOT" is 15. */
#define LS_SAFE_HINT_ACTION_MAX 20u

/* Buffer a caller must provide to ls_safe_hint_format(). */
#define LS_SAFE_HINT_TEXT_MAX 64u

/* What fits on one line of the narrowest panel we build for.
   480 px wide, less 8 px of screen padding each side and ~5 px of panel
   border plus padding each side, leaves 454 px. sdr_font_mono_sm() is
   Consolas 14, monospace, adv_w 123/16 = 7.69 px, so 59 characters fit.
   48 is that with room for letter spacing and a longer action caption; the
   720 px board is wider still. A hint that exceeds this wraps onto a second
   line and pushes the buttons down, which is the opposite of the point. */
#define LS_SAFE_HINT_MAX_CHARS 48u

typedef enum {
    /* Nothing has been pressed. State the rule, with the duration in it. */
    LS_SAFE_HINT_READY = 0,
    /* A hold is under way: name the action and the time still to run. */
    LS_SAFE_HINT_COUNTDOWN,
    /* Released early. Say so - this is the case that read as a dead button. */
    LS_SAFE_HINT_CANCELLED,
    /* The hold completed and the action fired. */
    LS_SAFE_HINT_CONFIRMED
} ls_safe_hint_phase_t;

typedef struct {
    ls_safe_hint_phase_t phase;
    char     action[LS_SAFE_HINT_ACTION_MAX];  /* caption being held */
    uint32_t hold_ms;                          /* the configured hold */
    uint32_t remaining_ms;                     /* quantised to tenths */
} ls_safe_hint_t;

void ls_safe_hint_init(ls_safe_hint_t *hint, uint32_t hold_ms);

/* Folds one confirmation effect into the hint. `action` is the caption of the
   control the effect came from; `remaining_ms` is
   ls_ui_confirm_remaining_ms() for that control at the same instant.
   Returns true when the rendered text would change, so the caller repaints
   on a tenth boundary rather than on every LV_EVENT_PRESSING. */
bool ls_safe_hint_update(ls_safe_hint_t *hint, ls_ui_confirm_effect_t effect,
                         const char *action, uint32_t remaining_ms);

/* Renders the hint into `out`, always NUL-terminated, never past `cap`.
   Returns the number of characters written. */
size_t ls_safe_hint_format(const ls_safe_hint_t *hint, char *out, size_t cap);

/* The role each phase is drawn in, so a countdown, a refusal and a
   confirmation are not three identical grey lines. */
ls_ui_color_role_t ls_safe_hint_color(const ls_safe_hint_t *hint);

#ifdef __cplusplus
}
#endif
