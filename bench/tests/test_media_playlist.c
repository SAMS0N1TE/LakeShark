/* LS_TEST_SOURCES: ${FW}/components/apps/media_gui/media_playlist.c */
/* LS_TEST_DEFINE: CONFIG_AUDIO_PLAYER_ENABLE_MP3=1 CONFIG_AUDIO_PLAYER_ENABLE_WAV=1 */
#include "ls_test.h"
#include "media_playlist.h"

#include <direct.h>
#include <stdio.h>
#include <stdlib.h>

static const char *fixture = ".ls_media_playlist_fixture";

static void fixture_path(char *path, size_t size, const char *name)
{
    snprintf(path, size, "%s/%s", fixture, name);
}

static void create_file(const char *name)
{
    char path[256];
    fixture_path(path, sizeof(path), name);
    FILE *file = fopen(path, "wb");
    LS_CHECK_MSG(file != NULL, "could not create fixture file %s", path);
    if (file) {
        fputs("fixture", file);
        fclose(file);
    }
}

static void remove_fixture(void)
{
    static const char *files[] = {
        "lake.mp3", "dispatch.WAV", "readme.txt", "lossless.flac",
        "podcast.m4a", "false.wav.txt"
    };
    char path[256];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        fixture_path(path, sizeof(path), files[i]);
        remove(path);
    }
    fixture_path(path, sizeof(path), "album.mp3");
    _rmdir(path);
    fixture_path(path, sizeof(path), "nested");
    _rmdir(path);
    _rmdir(fixture);
}

LS_CASE(mixed_directory_exposes_only_decodable_regular_files)
{
    remove_fixture();
    LS_EQ_INT(_mkdir(fixture), 0);

    create_file("lake.mp3");
    create_file("dispatch.WAV");
    create_file("readme.txt");
    create_file("lossless.flac");
    create_file("podcast.m4a");
    create_file("false.wav.txt");

    char path[256];
    fixture_path(path, sizeof(path), "album.mp3");
    LS_EQ_INT(_mkdir(path), 0);
    fixture_path(path, sizeof(path), "nested");
    LS_EQ_INT(_mkdir(path), 0);

    file_iterator_instance_t *playlist = ls_media_playlist_open(fixture);
    LS_CHECK(playlist != NULL);

    int saw_mp3 = 0;
    int saw_wav = 0;
    if (playlist) {
        /* AppMedia displays list[i] and hands the same i to play-by-index, so
           checking every slot covers both the visible and selected playlist. */
        LS_EQ_UINT(playlist->count, 2);
        for (size_t i = 0; i < playlist->count; ++i) {
            if (!strcmp(playlist->list[i], "lake.mp3")) saw_mp3++;
            else if (!strcmp(playlist->list[i], "dispatch.WAV")) saw_wav++;
            else LS_CHECK_MSG(0, "non-track at playlist index %u: %s",
                              (unsigned)i, playlist->list[i]);
        }
    }
    LS_EQ_INT(saw_mp3, 1);
    LS_EQ_INT(saw_wav, 1);

    ls_media_playlist_close(playlist);
    remove_fixture();
}
