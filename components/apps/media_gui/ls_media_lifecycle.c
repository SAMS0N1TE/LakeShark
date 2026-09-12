/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ls_media_lifecycle.h"

#include <stddef.h>

/**/
/* See the header for the why.  This translation unit is deliberately empty of
   IDF, LVGL and audio_player dependencies so the bench can link it as-is and
   verify the transition ordering with mocked hooks. */

static ls_media_lifecycle_hooks_t s_hooks;
static bool                       s_owner;

void ls_media_lifecycle_configure(const ls_media_lifecycle_hooks_t *hooks)
{
    if (hooks) {
        s_hooks = *hooks;
    } else {
        s_hooks.park_radio = NULL;
        s_hooks.stop_audio = NULL;
    }
}

void ls_media_lifecycle_enter(void)
{
    /* Idempotent: the shell can call run() or resume() while Music already
       thinks it owns the codec (e.g. a manual re-open of an app the shell
       never actually closed).  Parking twice would still be safe because
       app_park() bails on s_parked, but calling the hook once keeps the
       ordering test unambiguous. */
    if (s_owner) return;
    if (s_hooks.park_radio) s_hooks.park_radio();
    s_owner = true;
}

void ls_media_lifecycle_leave(void)
{
    /* Idempotent: close() may follow pause() with no re-enter, and the second
       call must not fire stop_audio again - a redundant audio_player_stop is
       cheap, but the test asserts that leave() with no matching enter is a
       no-op, which is the actual "one owner" guarantee. */
    if (!s_owner) return;
    if (s_hooks.stop_audio) s_hooks.stop_audio();
    s_owner = false;
}

bool ls_media_lifecycle_is_owner(void)
{
    return s_owner;
}

void ls_media_lifecycle_reset(void)
{
    s_hooks.park_radio = NULL;
    s_hooks.stop_audio = NULL;
    s_owner = false;
}
