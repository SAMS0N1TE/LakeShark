/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp_extra_player_state.h"

#include <stdio.h>
#include <string.h>

#include "audio_player.h"

/*LS-753*/
/* Store the iterator pointer as well as the index.  The pointer is stable
   while the iterator is live, and comparing both keeps a match on iterator A
   from claiming the same slot on iterator B after a source switch.

   For direct-file playback the module keeps the path (basename is derived by
   callers).  A "mode" tag distinguishes the two cases so an index query
   during path playback returns false, and vice versa. */
typedef enum {
    LS_PS_NONE,
    LS_PS_INDEX,
    LS_PS_PATH
} ls_player_mode_t;

static ls_player_mode_t          s_mode  = LS_PS_NONE;
static file_iterator_instance_t *s_iter  = NULL;
static int                       s_index = -1;
static char                      s_path[192] = {0};

void bsp_extra_player_state_note_play_index(file_iterator_instance_t *iter,
                                            int index)
{
    s_mode   = LS_PS_INDEX;
    s_iter   = iter;
    s_index  = index;
    s_path[0] = '\0';
}

void bsp_extra_player_state_note_play_path(const char *path)
{
    s_mode  = LS_PS_PATH;
    s_iter  = NULL;
    s_index = -1;
    if (path) snprintf(s_path, sizeof(s_path), "%s", path);
    else      s_path[0] = '\0';
}

void bsp_extra_player_state_reset(void)
{
    s_mode  = LS_PS_NONE;
    s_iter  = NULL;
    s_index = -1;
    s_path[0] = '\0';
}

static bool matches_index(file_iterator_instance_t *iter, int index)
{
    return s_mode == LS_PS_INDEX && s_index >= 0 &&
           iter == s_iter && index == s_index;
}

bool bsp_extra_player_state_is_playing_by_index(file_iterator_instance_t *iter,
                                                int index)
{
    if (!matches_index(iter, index)) return false;
    return audio_player_get_state() == AUDIO_PLAYER_STATE_PLAYING;
}

bool bsp_extra_player_state_is_active_by_index(file_iterator_instance_t *iter,
                                               int index)
{
    if (!matches_index(iter, index)) return false;
    audio_player_state_t st = audio_player_get_state();
    return st == AUDIO_PLAYER_STATE_PLAYING || st == AUDIO_PLAYER_STATE_PAUSE;
}

/*LS-753*/
const char *bsp_extra_player_state_active_path(void)
{
    if (s_mode != LS_PS_PATH || !s_path[0]) return NULL;
    audio_player_state_t st = audio_player_get_state();
    if (st != AUDIO_PLAYER_STATE_PLAYING && st != AUDIO_PLAYER_STATE_PAUSE)
        return NULL;
    return s_path;
}

bool bsp_extra_player_state_is_playing_path(void)
{
    if (s_mode != LS_PS_PATH) return false;
    return audio_player_get_state() == AUDIO_PLAYER_STATE_PLAYING;
}

bool bsp_extra_player_state_is_active_path(void)
{
    if (s_mode != LS_PS_PATH) return false;
    audio_player_state_t st = audio_player_get_state();
    return st == AUDIO_PLAYER_STATE_PLAYING || st == AUDIO_PLAYER_STATE_PAUSE;
}
