#pragma once

#include <stdbool.h>

#include "file_iterator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the iterator consumed by both the Music list and play-by-index. */
file_iterator_instance_t *ls_media_playlist_open(const char *directory_path);
void ls_media_playlist_close(file_iterator_instance_t *playlist);

/**/
/* True when `name` ends in an extension that the current audio_player build
   was compiled with a decoder for (MP3 and/or WAV via
   CONFIG_AUDIO_PLAYER_ENABLE_*).  Case-insensitive on the ASCII suffix.  The
   file browser calls this before handing a path to Music; the playlist
   scanner calls it while walking a directory.  If a caller advertises an
   audio format the player was not built with, this returns false so the
   caller can keep the user on the preview/properties path instead of
   launching a player that cannot decode the file. */
bool ls_media_supported_extension(const char *name);

#ifdef __cplusplus
}
#endif
