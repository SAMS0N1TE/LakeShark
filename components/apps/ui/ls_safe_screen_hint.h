#pragma once

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
