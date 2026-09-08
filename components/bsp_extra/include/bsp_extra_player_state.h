/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "file_iterator.h"

#ifdef __cplusplus
extern "C" {
#endif

/*LS-753*/
/* Playback bookkeeping for the Music app.  bsp_extra_player_is_playing_by_index
   used to compare the caller's index against file_iterator_get_index(), which
   nothing on this side ever sets - so track zero looked pinned to PLAYING and
   any other track looked stopped even while audio_player was decoding.  These
   entry points record what the caller asked to play and consult the actual
   audio_player state, and live in their own translation unit so the logic
   sits on the host bench without the codec/i2s stack.

   The module also tracks the "direct-file" case: a path handed to
   bsp_extra_player_play_file has no row in any playlist, but still owns the
   codec, so the Music UI needs it visible for STOP, PAUSE/RESUME and label. */
void bsp_extra_player_state_note_play_index(file_iterator_instance_t *iter,
                                            int index);
void bsp_extra_player_state_note_play_path(const char *path);
void bsp_extra_player_state_reset(void);

bool bsp_extra_player_state_is_playing_by_index(file_iterator_instance_t *iter,
                                                int index);
bool bsp_extra_player_state_is_active_by_index(file_iterator_instance_t *iter,
                                               int index);

/*LS-753*/
/* Direct-file (path-based) playback queries.  Return values are joined with
   the real audio_player state so a natural end or a stop is reflected without
   any explicit notification from the codec side. */
const char *bsp_extra_player_state_active_path(void);
bool bsp_extra_player_state_is_playing_path(void);
bool bsp_extra_player_state_is_active_path(void);

#ifdef __cplusplus
}
#endif
