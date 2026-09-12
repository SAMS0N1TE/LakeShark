/* LS_TEST_SOURCES: ${FW}/components/bsp_extra/src/bsp_extra_player_state.c */
#include "ls_test.h"
#include "audio_player.h"
#include "bsp_extra_player_state.h"
#include "file_iterator.h"

#include <string.h>

/**/
/* The old is_playing_by_index reduced to "index == iterator->index", and
   nothing on this side ever wrote iterator->index, so track zero looked
   pinned to playing and every other track looked stopped even while
   audio_player was decoding.  These cases pin the new behaviour: the answer
   comes from what was last handed to play_index() AND the real player state,
   and covers the natural-end transition asked for by the task. */

static file_iterator_instance_t make_iter(size_t count)
{
    file_iterator_instance_t iter;
    memset(&iter, 0, sizeof(iter));
    iter.count = count;
    iter.index = 0;
    return iter;
}

LS_CASE(nonzero_track_reports_playing_only_for_that_index)
{
    file_iterator_instance_t iter = make_iter(4);
    bsp_extra_player_state_reset();
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_IDLE);

    bsp_extra_player_state_note_play_index(&iter, 3);
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);

    /* The whole point: the answer must be right for a nonzero track. */
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter, 3));

    /* And the other slots must not claim the same playback. */
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter, 0));
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter, 1));
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter, 2));
}

LS_CASE(natural_end_reports_stopped)
{
    file_iterator_instance_t iter = make_iter(4);
    bsp_extra_player_state_reset();

    bsp_extra_player_state_note_play_index(&iter, 2);
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter, 2));

    /* audio_player runs off the end of the file and transitions to IDLE on
       its own; the Music UI must stop showing PAUSE, and the label must
       fall back to "stopped".  Nothing else in this module gets told. */
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_IDLE);
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter, 2));
    LS_CHECK(!bsp_extra_player_state_is_active_by_index(&iter, 2));
}

LS_CASE(pause_is_active_but_not_playing)
{
    file_iterator_instance_t iter = make_iter(4);
    bsp_extra_player_state_reset();

    bsp_extra_player_state_note_play_index(&iter, 1);
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter, 1));
    LS_CHECK(bsp_extra_player_state_is_active_by_index(&iter, 1));

    /* User taps PAUSE.  The button should read PLAY (resume), but the track
       is still loaded, so _playing must not be cleared - the UI relies on
       is_active_by_index to distinguish PAUSE from IDLE. */
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PAUSE);
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter, 1));
    LS_CHECK(bsp_extra_player_state_is_active_by_index(&iter, 1));

    /* Resume returns to PLAYING - both queries flip back. */
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter, 1));
    LS_CHECK(bsp_extra_player_state_is_active_by_index(&iter, 1));
}

LS_CASE(replacement_track_supersedes_previous)
{
    file_iterator_instance_t iter = make_iter(4);
    bsp_extra_player_state_reset();

    bsp_extra_player_state_note_play_index(&iter, 1);
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter, 1));

    /* Selecting a different row must move the "current" index. */
    bsp_extra_player_state_note_play_index(&iter, 3);
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter, 1));
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter, 3));
}

LS_CASE(stop_reports_stopped)
{
    file_iterator_instance_t iter = make_iter(4);
    bsp_extra_player_state_reset();

    bsp_extra_player_state_note_play_index(&iter, 2);
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter, 2));

    /* audio_player_stop() lands the state machine in IDLE.  Nothing on this
       side gets told; the query must return false from state alone. */
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_IDLE);
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter, 2));
    LS_CHECK(!bsp_extra_player_state_is_active_by_index(&iter, 2));
}

LS_CASE(playing_by_path_matches_no_index)
{
    file_iterator_instance_t iter = make_iter(4);
    bsp_extra_player_state_reset();

    bsp_extra_player_state_note_play_index(&iter, 2);
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter, 2));

    /* The FILES app opens a track by full path.  From the Music UI point of
       view no playlist row is playing - the file may not even live in the
       current source. */
    bsp_extra_player_state_note_play_path("/sdcard/other/x.mp3");
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter, 0));
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter, 2));
    LS_CHECK(!bsp_extra_player_state_is_active_by_index(&iter, 2));
}

LS_CASE(different_iterator_does_not_share_state)
{
    file_iterator_instance_t iter_a = make_iter(4);
    file_iterator_instance_t iter_b = make_iter(4);
    bsp_extra_player_state_reset();

    bsp_extra_player_state_note_play_index(&iter_a, 2);
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter_a, 2));

    /* Different iterator (different source), same index number: not the same
       playback. */
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter_b, 2));
    LS_CHECK(!bsp_extra_player_state_is_active_by_index(&iter_b, 2));
}

/**/
/* Direct-file playback: the Files-to-Music handoff hands a path to the
   player, and the Music UI has to see it so STOP, PAUSE/RESUME and close
   work.  Before the fix, note_play_path took no argument, the state module
   stored nothing, and _playing stayed at -1, so the UI showed "stopped" for
   a file the codec was busy decoding. */
LS_CASE(direct_path_reports_playing_paused_and_stopped)
{
    bsp_extra_player_state_reset();
    LS_CHECK(bsp_extra_player_state_active_path() == NULL);
    LS_CHECK(!bsp_extra_player_state_is_playing_path());
    LS_CHECK(!bsp_extra_player_state_is_active_path());

    bsp_extra_player_state_note_play_path("/sdcard/song/one.mp3");
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_playing_path());
    LS_CHECK(bsp_extra_player_state_is_active_path());
    LS_EQ_STR(bsp_extra_player_state_active_path(),
              "/sdcard/song/one.mp3");

    /* PAUSE: still loaded (active) but not playing.  UI must show ">" flip
       to "||" and the PLAY button label must switch. */
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PAUSE);
    LS_CHECK(!bsp_extra_player_state_is_playing_path());
    LS_CHECK(bsp_extra_player_state_is_active_path());
    LS_EQ_STR(bsp_extra_player_state_active_path(),
              "/sdcard/song/one.mp3");

    /* STOP or natural end: audio_player transitions to IDLE and the label
       falls back to "stopped" - active_path returns NULL from state alone,
       without any explicit notification. */
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_IDLE);
    LS_CHECK(!bsp_extra_player_state_is_playing_path());
    LS_CHECK(!bsp_extra_player_state_is_active_path());
    LS_CHECK(bsp_extra_player_state_active_path() == NULL);
}

/**/
/* Selecting a file from Files while an indexed track is playing must move
   ownership to path mode: the old index no longer claims the codec and the
   new file shows in the UI. */
LS_CASE(direct_path_supersedes_index_playback)
{
    file_iterator_instance_t iter = make_iter(4);
    bsp_extra_player_state_reset();

    bsp_extra_player_state_note_play_index(&iter, 2);
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter, 2));

    bsp_extra_player_state_note_play_path("/sdcard/direct.mp3");
    LS_CHECK(!bsp_extra_player_state_is_playing_by_index(&iter, 2));
    LS_CHECK(!bsp_extra_player_state_is_active_by_index(&iter, 2));
    LS_CHECK(bsp_extra_player_state_is_playing_path());
    LS_CHECK(bsp_extra_player_state_is_active_path());
    LS_EQ_STR(bsp_extra_player_state_active_path(), "/sdcard/direct.mp3");
}

/**/
/* Tapping a playlist row after direct-file playback flips ownership back to
   index mode - path queries must fall silent so the row-based UI takes over
   again. */
LS_CASE(index_supersedes_direct_path_playback)
{
    file_iterator_instance_t iter = make_iter(4);
    bsp_extra_player_state_reset();

    bsp_extra_player_state_note_play_path("/sdcard/first.mp3");
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_active_path());
    LS_EQ_STR(bsp_extra_player_state_active_path(), "/sdcard/first.mp3");

    bsp_extra_player_state_note_play_index(&iter, 1);
    LS_CHECK(!bsp_extra_player_state_is_playing_path());
    LS_CHECK(!bsp_extra_player_state_is_active_path());
    LS_CHECK(bsp_extra_player_state_active_path() == NULL);
    LS_CHECK(bsp_extra_player_state_is_playing_by_index(&iter, 1));
}

/**/
/* First handoff: run() consumes it - subsequent handoff: resume() consumes
   another one and the state module tracks the new path.  This is the
   state-machine face of the queue task's "first AND subsequent" clause. */
LS_CASE(subsequent_direct_path_handoff_replaces_previous)
{
    bsp_extra_player_state_reset();

    /* First handoff.  audio_player runs; Music shows it. */
    bsp_extra_player_state_note_play_path("/sdcard/one.mp3");
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_EQ_STR(bsp_extra_player_state_active_path(), "/sdcard/one.mp3");

    /* User backgrounds Music, picks a second file in Files.  Music resumes,
       consumes the new handoff, and note_play_path is called again with the
       new path - the state module must forget the first path entirely. */
    bsp_extra_player_state_note_play_path("/sdcard/two.mp3");
    LS_EQ_STR(bsp_extra_player_state_active_path(), "/sdcard/two.mp3");
    LS_CHECK(bsp_extra_player_state_is_playing_path());
}
