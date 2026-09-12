/* LS_TEST_CFLAGS: -Wno-format */
/* LS_TEST_LINK: -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=free -Wl,--wrap=strdup */
/* LS_TEST_SOURCES: ${FW}/managed_components/chmorgan__esp-file-iterator/file_iterator.c ${FW}/components/apps/media_gui/file_iterator_delete.c ${FW}/components/apps/media_gui/media_playlist.c */
/* LS_TEST_DEFINE: CONFIG_AUDIO_PLAYER_ENABLE_MP3=1 CONFIG_AUDIO_PLAYER_ENABLE_WAV=1 */
/**/
/* file_iterator_new allocates a struct, a path string, a pointer array and
   one string per entry.  file_iterator_delete is the missing counterpart -
   before the fix it was declared but never defined, so callers gave up on
   freeing entirely and every iterator lived forever.  This test wraps the
   libc allocator so we can count bytes-in vs bytes-out across a full create
   / destroy cycle and fail if the balance does not return to baseline.

   media_playlist_open / _close is exercised alongside because AppMedia is
   the only real consumer and it uses the playlist wrapper, not
   file_iterator_new directly. */

#include "ls_test.h"
#include "file_iterator.h"
#include "media_playlist.h"

#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wrapped by -Wl,--wrap= at link time.  __real_* resolve to the untouched
   libc symbols; __wrap_* replace every reference in the test binary. */
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void  __real_free(void *);

static long g_live_allocs;

void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);
    if (p) g_live_allocs++;
    return p;
}

void *__wrap_calloc(size_t a, size_t b)
{
    void *p = __real_calloc(a, b);
    if (p) g_live_allocs++;
    return p;
}

void __wrap_free(void *p)
{
    if (p) g_live_allocs--;
    __real_free(p);
}

/* strdup on UCRT calls its own internal allocator that ld --wrap does not
   catch, so route it through __wrap_malloc explicitly - the memory then
   frees through __wrap_free with matching bookkeeping. */
char *__wrap_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = __wrap_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static const char *fixture = ".ls_file_iterator_fixture";

static void fixture_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s/%s", fixture, name);
}

static void create_file(const char *name)
{
    char path[256];
    fixture_path(path, sizeof(path), name);
    FILE *f = fopen(path, "wb");
    LS_CHECK_MSG(f != NULL, "could not create fixture file %s", path);
    if (f) { fputs("x", f); fclose(f); }
}

static void remove_fixture(void)
{
    static const char *files[] = {
        "alpha.mp3", "beta.WAV", "gamma.txt", "delta.mp3"
    };
    char path[256];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        fixture_path(path, sizeof(path), files[i]);
        remove(path);
    }
    _rmdir(fixture);
}

static void make_fixture(void)
{
    remove_fixture();
    LS_EQ_INT(_mkdir(fixture), 0);
    create_file("alpha.mp3");
    create_file("beta.WAV");
    create_file("gamma.txt");
    create_file("delta.mp3");
}

/**/
/* The baseline check: file_iterator_new must acquire memory, and
   file_iterator_delete must give every byte back.  Sampling g_live_allocs
   before and after the pair is what proves the destructor is not lying. */
LS_CASE(file_iterator_delete_returns_to_baseline)
{
    make_fixture();

    long start = g_live_allocs;

    file_iterator_instance_t *iter = file_iterator_new(fixture);
    LS_CHECK(iter != NULL);
    LS_CHECK_MSG(g_live_allocs > start,
                 "file_iterator_new must acquire memory (start=%ld now=%ld)",
                 start, g_live_allocs);

    file_iterator_delete(iter);
    LS_EQ_INT(g_live_allocs, start);

    remove_fixture();
}

/**/
/* Same check for the wrapper AppMedia actually uses.  If media_playlist
   grows a new allocation site later, this test fails without anyone having
   to remember to update it. */
LS_CASE(media_playlist_close_returns_to_baseline)
{
    make_fixture();

    long start = g_live_allocs;

    file_iterator_instance_t *pl = ls_media_playlist_open(fixture);
    LS_CHECK(pl != NULL);
    LS_CHECK_MSG(g_live_allocs > start,
                 "ls_media_playlist_open must acquire memory (start=%ld now=%ld)",
                 start, g_live_allocs);

    ls_media_playlist_close(pl);
    LS_EQ_INT(g_live_allocs, start);

    remove_fixture();
}

/**/
/* Repeated open/close on the same directory must not drift either way -
   this catches a destructor that misses one allocation per call, which
   would only surface after hundreds of source switches on the device. */
LS_CASE(repeated_open_close_does_not_drift)
{
    make_fixture();

    long start = g_live_allocs;
    for (int i = 0; i < 16; ++i) {
        file_iterator_instance_t *iter = file_iterator_new(fixture);
        LS_CHECK(iter != NULL);
        file_iterator_delete(iter);
    }
    LS_EQ_INT(g_live_allocs, start);

    for (int i = 0; i < 16; ++i) {
        file_iterator_instance_t *pl = ls_media_playlist_open(fixture);
        LS_CHECK(pl != NULL);
        ls_media_playlist_close(pl);
    }
    LS_EQ_INT(g_live_allocs, start);

    remove_fixture();
}

/**/
/* file_iterator_delete(NULL) must be a no-op - the header does not promise
   otherwise, but every reasonable destructor accepts a null handle and the
   AppMedia loop over the two sources relies on that when one source never
   built. */
LS_CASE(delete_null_is_noop)
{
    long start = g_live_allocs;
    file_iterator_delete(NULL);
    ls_media_playlist_close(NULL);
    LS_EQ_INT(g_live_allocs, start);
}
