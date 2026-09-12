/* LS_TEST_SOURCES: ${FW}/components/apps/media_gui/media_playlist.c */
/* LS_TEST_DEFINE: CONFIG_AUDIO_PLAYER_ENABLE_MP3=1 CONFIG_AUDIO_PLAYER_ENABLE_WAV=1 */
#include "ls_test.h"
#include "media_playlist.h"

#include <stddef.h>

/**/

LS_CASE(mp3_extension_is_supported)
{
    LS_CHECK(ls_media_supported_extension("song.mp3"));
    LS_CHECK(ls_media_supported_extension("SONG.MP3"));
    LS_CHECK(ls_media_supported_extension("mixed.Mp3"));
}

LS_CASE(wav_extension_is_supported)
{
    LS_CHECK(ls_media_supported_extension("dispatch.wav"));
    LS_CHECK(ls_media_supported_extension("dispatch.WAV"));
    LS_CHECK(ls_media_supported_extension("dispatch.Wav"));
}

LS_CASE(m4a_extension_is_not_supported)
{
    /* The bundled player has no AAC/M4A decoder, so an .m4a must NOT be
       claimed by ls_media_supported_extension.  The old FileBrowser routed
       these to Music and produced an unknown-file-type failure. */
    LS_CHECK(!ls_media_supported_extension("podcast.m4a"));
    LS_CHECK(!ls_media_supported_extension("podcast.M4A"));
}

LS_CASE(flac_extension_is_not_supported)
{
    /* Same story for FLAC: no decoder in this build. */
    LS_CHECK(!ls_media_supported_extension("album.flac"));
    LS_CHECK(!ls_media_supported_extension("album.FLAC"));
}

LS_CASE(non_audio_extensions_are_not_supported)
{
    LS_CHECK(!ls_media_supported_extension("notes.txt"));
    LS_CHECK(!ls_media_supported_extension("image.png"));
    LS_CHECK(!ls_media_supported_extension("bin"));
    LS_CHECK(!ls_media_supported_extension("archive.tar.gz"));
}

LS_CASE(name_without_extension_is_not_supported)
{
    LS_CHECK(!ls_media_supported_extension("README"));
    LS_CHECK(!ls_media_supported_extension(""));
}

LS_CASE(false_extension_prefix_is_rejected)
{

    LS_CHECK(!ls_media_supported_extension("false.wav.txt"));
    LS_CHECK(!ls_media_supported_extension("song.mp3.bak"));
}

LS_CASE(null_name_is_not_supported)
{
    LS_CHECK(!ls_media_supported_extension(NULL));
}
