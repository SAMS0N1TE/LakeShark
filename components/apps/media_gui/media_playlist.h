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

bool ls_media_supported_extension(const char *name);

#ifdef __cplusplus
}
#endif
