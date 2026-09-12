/* LS_TEST_SOURCES: ${FW}/components/apps/file_browser/file_browser_path.c */
#include "ls_test.h"
#include "file_browser_path.h"

#include <string.h>

/**/
/* Files-to-Music path handoff.  The old FileBrowser::openEntry glued
   "/sdcard/" onto a cwd that was already "/sdcard/..." or "/spiffs/...",
   producing paths like "/sdcard//sdcard/music/song.mp3" and silently
   rerouting every SPIFFS selection onto SD.  These cases pin the joiner to
   the rule: cwd is authoritative, and the browsed path is passed through
   unchanged.  If somebody re-introduces a root prepend, the SPIFFS cases
   below fail first. */

LS_CASE(sdcard_top_level_file)
{
    char buf[192] = {0};
    int n = file_browser_join_path("/sdcard", "song.mp3", buf, sizeof(buf));
    LS_CHECK(n > 0);
    LS_EQ_STR(buf, "/sdcard/song.mp3");
    LS_CHECK(strstr(buf, "//") == NULL);
}

LS_CASE(sdcard_nested_file)
{
    char buf[192] = {0};
    int n = file_browser_join_path("/sdcard/music", "song.mp3",
                                   buf, sizeof(buf));
    LS_CHECK(n > 0);
    LS_EQ_STR(buf, "/sdcard/music/song.mp3");
    /* The exact "//sdcard" fingerprint of the old double-root bug. */
    LS_CHECK(strstr(buf, "//") == NULL);
    LS_CHECK(strstr(buf, "/sdcard/sdcard") == NULL);
}

LS_CASE(sdcard_deeply_nested_file)
{
    char buf[192] = {0};
    int n = file_browser_join_path("/sdcard/music/albums/live",
                                   "track01.flac", buf, sizeof(buf));
    LS_CHECK(n > 0);
    LS_EQ_STR(buf, "/sdcard/music/albums/live/track01.flac");
    LS_CHECK(strstr(buf, "//") == NULL);
}

LS_CASE(spiffs_top_level_file)
{
    char buf[192] = {0};
    int n = file_browser_join_path("/spiffs", "boot.wav", buf, sizeof(buf));
    LS_CHECK(n > 0);
    /* SPIFFS must survive as SPIFFS - the old code rewrote this as
       "/sdcard//spiffs/boot.wav" and the player then failed to open it. */
    LS_EQ_STR(buf, "/spiffs/boot.wav");
    LS_CHECK(strstr(buf, "/sdcard") == NULL);
}

LS_CASE(spiffs_nested_file)
{
    char buf[192] = {0};
    int n = file_browser_join_path("/spiffs/tones", "alert.mp3",
                                   buf, sizeof(buf));
    LS_CHECK(n > 0);
    LS_EQ_STR(buf, "/spiffs/tones/alert.mp3");
    LS_CHECK(strstr(buf, "//") == NULL);
    LS_CHECK(strstr(buf, "/sdcard") == NULL);
}

LS_CASE(roots_view_passes_root_through)
{
    /* When cwd is empty the browser is showing the roots list; picking a row
       must produce the root path itself, not have any prefix added. */
    char buf[192] = {0};
    int n = file_browser_join_path("", "/sdcard", buf, sizeof(buf));
    LS_CHECK(n > 0);
    LS_EQ_STR(buf, "/sdcard");

    n = file_browser_join_path("", "/spiffs", buf, sizeof(buf));
    LS_CHECK(n > 0);
    LS_EQ_STR(buf, "/spiffs");
}

LS_CASE(null_cwd_is_treated_as_empty)
{
    char buf[192] = {0};
    int n = file_browser_join_path(NULL, "/sdcard", buf, sizeof(buf));
    LS_CHECK(n > 0);
    LS_EQ_STR(buf, "/sdcard");
}

LS_CASE(bad_arguments_return_error)
{
    char buf[32] = {0};
    LS_CHECK(file_browser_join_path("/sdcard", NULL, buf, sizeof(buf)) < 0);
    LS_CHECK(file_browser_join_path("/sdcard", "x", NULL, sizeof(buf)) < 0);
    LS_CHECK(file_browser_join_path("/sdcard", "x", buf, 0) < 0);
}

LS_CASE(truncation_returns_error_not_partial_path)
{

    char buf[16] = {0};
    int n = file_browser_join_path("/sdcard/music", "a_very_long_song.mp3",
                                   buf, sizeof(buf));
    LS_CHECK(n < 0);
}
