/* LS_TEST_SOURCES: ${FW}/components/apps/media_gui/ls_media_handoff.c */
#include "ls_test.h"
#include "ls_media_handoff.h"

#include <string.h>

/**/
/* Files-to-Music handoff.  Before the fix, the pending path lived in a
   private buffer inside AppMedia and only AppMedia::run consumed it - so
   once the shell had built Music, every later file pick from Files landed
   on AppMedia::resume and the path never played.  The module now owns the
   pending path; run() and resume() both call take(). */

LS_CASE(empty_take_returns_false)
{
    ls_media_handoff_reset();
    char buf[64] = "sentinel";
    LS_CHECK(!ls_media_handoff_take(buf, sizeof(buf)));
    /* take() must not touch the buffer when there is nothing pending. */
    LS_EQ_STR(buf, "sentinel");
}

LS_CASE(set_then_take_returns_path_and_clears)
{
    ls_media_handoff_reset();
    ls_media_handoff_set("/sdcard/a.mp3");
    char buf[64] = {0};
    LS_CHECK(ls_media_handoff_take(buf, sizeof(buf)));
    LS_EQ_STR(buf, "/sdcard/a.mp3");
    /* One-shot: a manual open of Music later must not re-fire the handoff. */
    LS_CHECK(!ls_media_handoff_take(buf, sizeof(buf)));
}

LS_CASE(second_set_overwrites_pending)
{
    ls_media_handoff_reset();
    ls_media_handoff_set("/sdcard/a.mp3");
    ls_media_handoff_set("/sdcard/b.mp3");
    char buf[64] = {0};
    LS_CHECK(ls_media_handoff_take(buf, sizeof(buf)));
    /* The second selection is what the user asked for - the first is stale. */
    LS_EQ_STR(buf, "/sdcard/b.mp3");
}

LS_CASE(set_null_clears_pending)
{
    ls_media_handoff_set("/sdcard/a.mp3");
    ls_media_handoff_set(NULL);
    char buf[64] = "sentinel";
    LS_CHECK(!ls_media_handoff_take(buf, sizeof(buf)));
    LS_EQ_STR(buf, "sentinel");
}

LS_CASE(first_and_subsequent_handoffs_both_fire)
{
    /* State-machine walk of the queue-task defect:
       - Music not built yet.  Files sets path A, launchByName("MUSIC") lands
         in AppMedia::run, which calls take() and plays A.
       - User goes back to Files, picks path B.  Files sets path B,
         launchByName("MUSIC") lands in AppMedia::resume (Music already
         built), which calls take() and plays B.  The old code did nothing
         on that second selection. */
    ls_media_handoff_reset();

    /* First handoff. */
    ls_media_handoff_set("/sdcard/first.mp3");
    char first[64] = {0};
    LS_CHECK(ls_media_handoff_take(first, sizeof(first)));
    LS_EQ_STR(first, "/sdcard/first.mp3");

    /* Nothing pending between the two picks. */
    char nothing[64] = "sentinel";
    LS_CHECK(!ls_media_handoff_take(nothing, sizeof(nothing)));
    LS_EQ_STR(nothing, "sentinel");

    /* Subsequent handoff. */
    ls_media_handoff_set("/sdcard/second.mp3");
    char second[64] = {0};
    LS_CHECK(ls_media_handoff_take(second, sizeof(second)));
    LS_EQ_STR(second, "/sdcard/second.mp3");

    /* And, again, no residual - the next manual open of Music must not
       hijack the user onto second.mp3. */
    LS_CHECK(!ls_media_handoff_take(second, sizeof(second)));
}

LS_CASE(long_path_is_truncated_not_overrun)
{
    ls_media_handoff_reset();
    char big[512];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    ls_media_handoff_set(big);
    char out[512] = {0};
    LS_CHECK(ls_media_handoff_take(out, sizeof(out)));
    /* Truncation is fine; the point is that the store never wrote past its
       own buffer and returned a NUL-terminated string. */
    LS_CHECK(strlen(out) > 0);
    LS_CHECK(strlen(out) < sizeof(big) - 1);
    LS_CHECK(out[strlen(out)] == '\0');
}
