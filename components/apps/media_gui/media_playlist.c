#include "media_playlist.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

/*LS-906*/
/* The bundled player only builds the decoders selected by these Kconfig
   options. The old unfiltered file_iterator made '.', directories, and files
   for absent decoders selectable, so PLAY usually attempted to fopen a
   directory. Build the iterator from regular files that its decoder can
   actually consume; the UI and play-by-index then share the same indices. */

static bool ascii_extension_equal(const char *actual, const char *expected)
{
    while (*actual && *expected) {
        char a = *actual++;
        char b = *expected++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return *actual == '\0' && *expected == '\0';
}

bool ls_media_supported_extension(const char *name)
{
    if (!name) return false;
    const char *extension = strrchr(name, '.');
    if (!extension) return false;

#if defined(CONFIG_AUDIO_PLAYER_ENABLE_MP3) && CONFIG_AUDIO_PLAYER_ENABLE_MP3
    if (ascii_extension_equal(extension, ".mp3")) return true;
#endif
#if defined(CONFIG_AUDIO_PLAYER_ENABLE_WAV) && CONFIG_AUDIO_PLAYER_ENABLE_WAV
    if (ascii_extension_equal(extension, ".wav")) return true;
#endif
    return false;
}

static bool playable_entry(const char *directory_path, const char *name)
{
    if (!name || !strcmp(name, ".") || !strcmp(name, "..") ||
        !ls_media_supported_extension(name)) {
        return false;
    }

    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s", directory_path, name);
    if (length < 0 || (size_t)length >= sizeof(path)) return false;

    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static void *playlist_alloc(size_t size, unsigned capabilities)
{
    return heap_caps_malloc(size, capabilities | MALLOC_CAP_8BIT);
}

static char *playlist_strdup(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = playlist_alloc(size, MALLOC_CAP_SPIRAM);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static void playlist_free_partial(file_iterator_instance_t *playlist, size_t names)
{
    if (!playlist) return;
    for (size_t i = 0; i < names; ++i) heap_caps_free(playlist->list[i]);
    heap_caps_free(playlist->list);
    heap_caps_free((void *)playlist->directory_path);
    heap_caps_free(playlist);
}

file_iterator_instance_t *ls_media_playlist_open(const char *directory_path)
{
    if (!directory_path) return NULL;

    DIR *directory = opendir(directory_path);
    if (!directory) return NULL;

    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (playable_entry(directory_path, entry->d_name)) ++count;
    }
    closedir(directory);

    file_iterator_instance_t *playlist = playlist_alloc(
        sizeof(*playlist), MALLOC_CAP_INTERNAL);
    if (!playlist) return NULL;
    memset(playlist, 0, sizeof(*playlist));

    playlist->directory_path = playlist_strdup(directory_path);
    if (!playlist->directory_path) {
        playlist_free_partial(playlist, 0);
        return NULL;
    }

    if (count > 0) {
        playlist->list = playlist_alloc(count * sizeof(*playlist->list),
                                        MALLOC_CAP_SPIRAM);
        if (!playlist->list) {
            playlist_free_partial(playlist, 0);
            return NULL;
        }
    }

    directory = opendir(directory_path);
    if (!directory) {
        playlist_free_partial(playlist, 0);
        return NULL;
    }

    size_t used = 0;
    while (used < count && (entry = readdir(directory)) != NULL) {
        if (!playable_entry(directory_path, entry->d_name)) continue;
        playlist->list[used] = playlist_strdup(entry->d_name);
        if (!playlist->list[used]) {
            closedir(directory);
            playlist_free_partial(playlist, used);
            return NULL;
        }
        ++used;
    }
    closedir(directory);

    /* A directory may change between the counting and filling passes. A file
       removed in that window simply shortens this snapshot; additions appear
       on the next scan. */
    playlist->count = used;
    playlist->index = 0;
    return playlist;
}

void ls_media_playlist_close(file_iterator_instance_t *playlist)
{
    playlist_free_partial(playlist, playlist ? playlist->count : 0);
}
