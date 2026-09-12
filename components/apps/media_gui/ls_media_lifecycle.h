/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**/
/* Codec ownership transitions for the Music app. */

typedef void (*ls_media_lifecycle_hook_t)(void);

typedef struct {
    ls_media_lifecycle_hook_t park_radio;
    ls_media_lifecycle_hook_t stop_audio;
} ls_media_lifecycle_hooks_t;

/* Firmware wires these once at startup; passing NULL forgets them (used only
   by the bench between cases). */
void ls_media_lifecycle_configure(const ls_media_lifecycle_hooks_t *hooks);

/* Called at the top of AppMedia::run() and AppMedia::resume(), BEFORE the
   player is touched.  Idempotent: two enters in a row park once. */
void ls_media_lifecycle_enter(void);

/* Called from AppMedia::pause(), AppMedia::background() and AppMedia::close()
   before the shell hands control to the next app.  Idempotent: a leave with
   no matching enter is a no-op. */
void ls_media_lifecycle_leave(void);

/* True after an enter() and before the matching leave().  Firmware ignores
   this; the bench uses it to assert single-owner semantics. */
bool ls_media_lifecycle_is_owner(void);

/* Test-only: forget any configured hooks and ownership state so a case starts
   from a known slate.  Firmware never calls this. */
void ls_media_lifecycle_reset(void);

#ifdef __cplusplus
}
#endif
