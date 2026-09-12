#pragma once

/* A framed, full-screen diagnostic report with two deliberate actions, built from the shared kit. */

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

#define LS_SAFE_SCREEN_HOLD_MS 1500u

/* Builds into parent (normally lv_scr_act()). Returns the root object, or
   NULL if cfg is NULL. */
lv_obj_t *ls_safe_screen_create(lv_obj_t *parent, const ls_safe_screen_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
