/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*LS-754*/
/* Codec ownership transitions for the Music app.  Music must be the ONE thing
   writing to the i2s codec while it is up: the radio decoders assume they own
   it, and audio_player writes decoded PCM into the same output.  Two writers
   is a crackle at best and an i2s critical-section fault at worst (LS-744).

   Normal route into Music is P25 or FM -> Files -> Music, and Files is passive
   (LS-604), so the radio decoder is only backgrounded when Music opens - its
   timer is paused but the radio pipe is still running and its on_exit was
   never called.  ls_media_lifecycle_enter() PARKS THE RADIO, so nothing else
   can still be writing to the codec by the time Music initialises the player.

   Leaving Music into a radio app used to just pause Music's LVGL timer and
   leave audio_player decoding into the codec the radio app was about to
   reopen; ls_media_lifecycle_leave() STOPS THE PLAYER, so leaving Music into
   any next app cannot leave media audio playing over it.

   The two operations are injected as hooks so the host bench can verify the
   ordering without linking against lakeshark_backend or audio_player.  The
   firmware wires them to lakeshark_radio_park() and audio_player_stop();
   the bench wires them to recorders that capture the call sequence.

   The module keeps enough state that a paired leave() only stops audio when a
   corresponding enter() said Music was the owner - so a redundant close() on
   an already-torn-down instance is a no-op, and enter() being called twice in
   a row does not double-park either. */

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
