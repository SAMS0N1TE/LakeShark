#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

/*LS-990  Pull-down shade for actions the operator wants without a laptop and
   without hunting for a hidden button. Opens from a swipe-down anywhere; the
   status bar no longer navigates. Actions are supplied by the host as function
   pointers so this file compiles clean in a headless build too - main/ owns
   screenshot.c and ls_wifi.c and those symbols do not exist on the nano. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Take a screenshot and, on success, write its filesystem path to
       path_out. Must run under the LVGL lock; the shade guarantees this and
       hides its own widgets before the call so nothing here appears in the
       image. */
    bool (*capture)(char *path_out, size_t path_len);

    /* SoftAP file server: is it up, and toggle it. */
    bool (*wifi_running)(void);
    void (*wifi_toggle)(void);

    /* Fills a short "where to fetch it from" hint that the confirmation names
       (e.g. "http://192.168.4.1/"). Empty string when nothing is up. */
    void (*fetch_hint)(char *out, size_t cap);

    /* Return to HOME. The shade closes first, then invokes this. */
    void (*go_home)(void);
} ls_shade_hooks_t;

/* Registers the callbacks the shade will call. NULL disables actions. */
void ls_shade_configure(const ls_shade_hooks_t *hooks);

/* Builds the shade and confirmation widgets under parent. Idempotent. */
void ls_shade_build(lv_obj_t *parent);

/* Show/hide/query. Safe to call before build(). */
void ls_shade_open(void);
void ls_shade_close(void);
bool ls_shade_is_open(void);

#ifdef __cplusplus
}
#endif
