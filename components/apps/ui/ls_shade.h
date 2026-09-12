#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

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
    /* Optional board orientation control. */
    void (*rotate)(void);
    /* Optional direct orientation, degrees 0/90/180/270. Takes precedence
       over the legacy cycle action. The host queues the actual change. */
    void (*set_orientation)(unsigned degrees);
} ls_shade_hooks_t;

/* Registers the callbacks the shade will call. NULL disables actions. */
void ls_shade_configure(const ls_shade_hooks_t *hooks);

/* Builds the shade and confirmation widgets under parent. Idempotent. */
void ls_shade_build(lv_obj_t *parent);
void ls_shade_resize(void);
void ls_shade_set_safe_insets(int horizontal, int vertical);

/* Show/hide/query. Safe to call before build(). */
void ls_shade_open(void);
void ls_shade_close(void);
bool ls_shade_is_open(void);

#ifdef __cplusplus
}
#endif
