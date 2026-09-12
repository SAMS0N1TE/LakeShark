/* LS_TEST_CFLAGS: -Wno-format */
/* LS_TEST_LINK: -Wl,--wrap=fopen -Wl,--wrap=fclose */
/* LS_TEST_SOURCES: ${FW}/components/bsp_extra/src/bsp_extra_player_open.c ${FW}/components/bsp_extra/src/bsp_extra_player_state.c ${FW}/managed_components/chmorgan__esp-file-iterator/file_iterator.c ${FW}/components/apps/media_gui/file_iterator_delete.c */
/**/
/* audio_player_play(fp) documents that it takes ownership of the FILE* only on ESP_OK - a non-OK return means the audio task never claimed it and the caller must fclose. */

#include "ls_test.h"
#include "audio_player.h"
#include "file_iterator.h"
#include "bsp_extra_player_state.h"

#include <direct.h>
#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>

esp_err_t bsp_extra_player_play_index(file_iterator_instance_t *instance,
                                      int index);
esp_err_t bsp_extra_player_play_file(const char *file_path);

/* -------- libc wrap: count fopen/fclose pairs from inside the binary. -- */

extern FILE *__real_fopen(const char *, const char *);
extern int   __real_fclose(FILE *);

static long g_files_open;

FILE *__wrap_fopen(const char *path, const char *mode)
{
    FILE *f = __real_fopen(path, mode);
    if (f) g_files_open++;
    return f;
}

int __wrap_fclose(FILE *f)
{
    if (f) g_files_open--;
    return __real_fclose(f);
}

/* -------- audio_player stub whose result the test controls. ------------ */

static esp_err_t s_play_result;
static int       s_play_calls;
static FILE     *s_last_fp;

esp_err_t audio_player_play(FILE *fp)
{
    s_play_calls++;
    s_last_fp = fp;
    return s_play_result;
}

/* -------- fixture: a real directory the play call can fopen. ---------- */

static char g_dir[96];

static void fixture_init(void)
{
    if (g_dir[0] == '\0') {
        snprintf(g_dir, sizeof(g_dir), ".ls_player_open_fixture_%lu",
                 (unsigned long)_getpid());
    }
}

static void write_file(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *f = __real_fopen(path, "wb");
    LS_CHECK_MSG(f != NULL, "could not create fixture file %s", path);
    if (f) { fputs("x", f); __real_fclose(f); }
}

static bool fixture_path_is_gone(const char *path)
{
    struct _stat st;
    errno = 0;
    return _stat(path, &st) != 0 && errno == ENOENT;
}

static bool fixture_remove_file(const char *path)
{
    int last_error = 0;

    for (int attempt = 0; attempt < 20; ++attempt) {
        errno = 0;
        if (remove(path) == 0 || errno == ENOENT) {
            if (fixture_path_is_gone(path)) return true;
        }
        last_error = errno;
        Sleep(10);
    }

    LS_CHECK_MSG(false, "could not clear fixture file %s after retries: %s",
                 path, strerror(last_error));
    return false;
}

static bool fixture_remove_dir(const char *path)
{
    int last_error = 0;

    for (int attempt = 0; attempt < 20; ++attempt) {
        errno = 0;
        if (_rmdir(path) == 0 || errno == ENOENT) {
            if (fixture_path_is_gone(path)) return true;
        }
        last_error = errno;
        Sleep(10);
    }

    LS_CHECK_MSG(false, "could not clear fixture directory %s after retries: %s",
                 path, strerror(last_error));
    return false;
}

static bool remove_fixture(void)
{
    static const char *names[] = { "one.mp3", "two.mp3" };
    bool cleared = true;

    fixture_init();
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", g_dir, names[i]);
        if (!fixture_remove_file(path)) cleared = false;
    }
    if (!fixture_remove_dir(g_dir)) cleared = false;
    return cleared;
}

static bool make_fixture(void)
{
    /* a locked one.mp3 reproduced the intermittent gate failure as
       _mkdir plus file-create errors and a misleading 16/32 play-call count.
       Isolate each process, retry Windows deletion, and stop at a direct
       cleanup diagnostic instead of continuing with a partial fixture. */
    if (!remove_fixture()) return false;
    errno = 0;
    if (_mkdir(g_dir) != 0) {
        LS_CHECK_MSG(false, "could not create fixture directory %s: %s",
                     g_dir, strerror(errno));
        return false;
    }
    write_file("one.mp3");
    write_file("two.mp3");
    return true;
}

/* Hand-built iterator so a case can point index 0 at a known filename
   without depending on the file_iterator's own scan order (which on this
   platform includes "." and "..") - the fix under test is the FILE*
   handoff in bsp_extra_player_play_index, not the directory scanner. */
static char *g_names[2];
static char  g_name0[64] = "one.mp3";
static char  g_name1[64] = "two.mp3";

static file_iterator_instance_t make_iter(void)
{
    g_names[0] = g_name0;
    g_names[1] = g_name1;
    file_iterator_instance_t it;
    memset(&it, 0, sizeof(it));
    it.count = 2;
    it.index = 0;
    it.list  = g_names;
    it.directory_path = g_dir;
    return it;
}

static void reset_state(void)
{
    g_files_open  = 0;
    s_play_calls  = 0;
    s_last_fp     = NULL;
    s_play_result = ESP_OK;
    bsp_extra_player_state_reset();
    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_IDLE);
}

/* -------- cases -------------------------------------------------------- */

LS_CASE(play_index_enqueue_failure_closes_the_file)
{
    if (!make_fixture()) return;
    reset_state();
    file_iterator_instance_t it = make_iter();

    s_play_result = ESP_FAIL;

    esp_err_t rc = bsp_extra_player_play_index(&it, 0);
    LS_EQ_INT(rc, ESP_FAIL);
    LS_EQ_INT(s_play_calls, 1);

    LS_EQ_INT(g_files_open, 0);

    /* And the state module must not be claiming playback started. */
    LS_CHECK(!bsp_extra_player_state_is_active_by_index(&it, 0));

    (void)remove_fixture();
}

/* Same guarantee for the direct-file entry point used by the Files ->
   Music handoff.  audio_player_play returning non-OK must close the fp. */
LS_CASE(play_file_enqueue_failure_closes_the_file)
{
    if (!make_fixture()) return;
    reset_state();

    s_play_result = ESP_FAIL;
    char path[256];
    snprintf(path, sizeof(path), "%s/one.mp3", g_dir);

    esp_err_t rc = bsp_extra_player_play_file(path);
    LS_EQ_INT(rc, ESP_FAIL);
    LS_EQ_INT(s_play_calls, 1);
    LS_EQ_INT(g_files_open, 0);
    LS_CHECK(bsp_extra_player_state_active_path() == NULL);

    (void)remove_fixture();
}

/* The device-side symptom is repeated taps stacking leaks until fatfs runs
   out of slots.  Loop the failure many times and check that the leak
   accounting never drifts above zero at any point. */
LS_CASE(repeated_enqueue_failures_do_not_accumulate)
{
    if (!make_fixture()) return;
    reset_state();
    file_iterator_instance_t it = make_iter();

    s_play_result = ESP_FAIL;

    for (int i = 0; i < 32; ++i) {
        (void)bsp_extra_player_play_index(&it, i & 1);
        /* After each rejected tap the tree must be back to zero opens -
           this is stricter than checking at the end because it catches a
           regression that only balances by coincidence. */
        LS_EQ_INT(g_files_open, 0);
    }
    LS_EQ_INT(s_play_calls, 32);

    (void)remove_fixture();
}

/* The happy path must NOT close the fp - audio_player owns it from that
   point and Music depends on the state module recording the index that
   the codec is decoding. */
LS_CASE(play_index_success_keeps_the_file_open)
{
    if (!make_fixture()) return;
    reset_state();
    file_iterator_instance_t it = make_iter();

    s_play_result = ESP_OK;

    esp_err_t rc = bsp_extra_player_play_index(&it, 0);
    LS_EQ_INT(rc, ESP_OK);
    LS_EQ_INT(s_play_calls, 1);

    /* audio_player took the fp - it stays open until the audio task
       finishes with it.  On the bench there is no audio task, so we
       close it ourselves to balance the wrap counter. */
    LS_EQ_INT(g_files_open, 1);
    LS_CHECK(s_last_fp != NULL);

    ls_shim_audio_state_set(AUDIO_PLAYER_STATE_PLAYING);
    LS_CHECK(bsp_extra_player_state_is_active_by_index(&it, 0));

    fclose(s_last_fp);
    LS_EQ_INT(g_files_open, 0);

    (void)remove_fixture();
}
