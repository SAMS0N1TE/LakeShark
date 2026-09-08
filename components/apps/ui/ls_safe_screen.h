#pragma once

/* LS-994  A framed, full-screen diagnostic report with two deliberate
   actions, built from the shared kit.

   It lives in ui/ rather than in main/ because nothing about it is specific
   to safe mode: it takes a title, a block of pre-formatted text and up to two
   labelled hold-to-confirm actions. Anything that has to stop and tell the
   operator something with no shell running - a fatal storage error, a failed
   radio bring-up - wants exactly this and should call it rather than
   hand-rolling a second one. It knows nothing about reset reasons or the boot
   sequence; the caller formats the text.

   Every size is display-relative (lv_pct / flex through the shared kit), so
   it fills a 1024x600 panel and a 720x720 one without a literal anywhere. */

#include "lvgl.h"
#include "ui/ls_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title;       /* header text, e.g. "SAFE MODE" */
    const char *headline;    /* one line under the header; may be NULL */
    const char *body;        /* multi-line report; may be NULL */
    const char *footer;      /* console hint or similar; may be NULL */
    const char *primary;     /* primary action label; NULL to omit */
    const char *secondary;   /* secondary action label; NULL to omit */
    void (*on_primary)(void);
    void (*on_secondary)(void);
    uint32_t hold_ms;        /* 0 -> LS_SAFE_SCREEN_HOLD_MS */
} ls_safe_screen_cfg_t;

/* Long enough that a stray touch on a board being handled cannot reboot it,
   short enough that an operator does not think the control is dead.

   LS-1010: an operator DID think the control was dead - a tap on TRY NORMAL
   BOOT "appears to do nothing" - so the duration is now stated on the screen
   above the buttons before anything is pressed, and the hold is narrated
   while it runs. See ui/ls_safe_screen_hint.h. The hold itself is unchanged;
   the fix was telling the operator about it, not shortening it. */
#define LS_SAFE_SCREEN_HOLD_MS 1500u

/* Builds into parent (normally lv_scr_act()). Returns the root object, or
   NULL if cfg is NULL. */
lv_obj_t *ls_safe_screen_create(lv_obj_t *parent, const ls_safe_screen_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
