/* LS_TEST_SOURCES: ${APP}/rec/rec_unique_name.c */
/**/
/* Before this fix, AppREC::saveCb built a base name from the process-local
   capture counter ("rec%03lu") and rec_save opened "<dir>/<name>.sub" with
   fopen("w").  After a reboot, a counter wrap, or the deletion of a NEWER
   capture the same base could name an OLDER file that was still on disk,
   and "w" truncated it silently - no dialog, no confirmation, and the only
   trace left in the FILES tab was that the byte size changed.

   The picker is a pure classifier that walks candidate suffixes until it
   finds a name whose ".sub" is absent, so the caller cannot pave over an
   existing capture without renaming it.  The bench pre-creates rec000.sub
   in a temp directory and asserts every collision case the recorder can
   present at runtime: fresh name goes through untouched, a taken name
   walks to "rec000-1", a run of taken names walks to the next free one,
   and - critically - the existing capture's bytes are still on disk after
   the picker has run. */

#include "ls_test.h"
#include "rec_unique_name.h"
#include "rec_file_open.h"

#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *fixture = ".ls_rec_unique_fixture";

static void fx_path(char *out, size_t len, const char *name)
{
    snprintf(out, len, "%s/%s.sub", fixture, name);
}

static void fx_write(const char *name, const char *body)
{
    char path[160];
    fx_path(path, sizeof(path), name);
    FILE *f = fopen(path, "wb");
    LS_CHECK_MSG(f != NULL, "could not create fixture %s", path);
    if (f) { fputs(body, f); fclose(f); }
}

static long fx_size(const char *name)
{
    char path[160];
    fx_path(path, sizeof(path), name);
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static int fx_read(const char *name, char *out, size_t len)
{
    char path[160];
    fx_path(path, sizeof(path), name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, len - 1, f);
    fclose(f);
    out[n] = '\0';
    return (int)n;
}

static void fx_rm_all(void)
{
    /* Wide enough net to cover every candidate the picker may leave behind
       in a rerun of the suite. */
    char path[160];
    static const char *bases[] = { "rec000", "rec001", "rec042", "capture" };
    for (size_t i = 0; i < sizeof(bases)/sizeof(bases[0]); i++) {
        fx_path(path, sizeof(path), bases[i]);
        remove(path);
        for (int j = 1; j <= REC_UNIQUE_MAX_SUFFIX; j++) {
            char nm[64];
            snprintf(nm, sizeof(nm), "%s-%d", bases[i], j);
            fx_path(path, sizeof(path), nm);
            remove(path);
        }
    }
    _rmdir(fixture);
}

static void fx_setup(void)
{
    fx_rm_all();
    LS_EQ_INT(_mkdir(fixture), 0);
}

LS_CASE(fresh_name_is_returned_verbatim)
{
    /* No collision - the picker must return the base as-is so the current
       "rec%03lu" naming keeps its shape when nothing is in the way. */
    fx_setup();

    char out[40] = {0};
    LS_EQ_INT(rec_pick_unique_name(fixture, "rec000", ".sub",
                                   out, sizeof(out)), 0);
    LS_EQ_STR(out, "rec000");

    fx_rm_all();
}

LS_CASE(collision_walks_to_next_suffix)
{
    /* The done-when case: rec000.sub already exists (from an older boot),
       the counter has reset, and SAVE would otherwise truncate it.  The
       picker must land on rec000-1 instead. */
    fx_setup();
    fx_write("rec000", "old capture bytes\n");

    char out[40] = {0};
    LS_EQ_INT(rec_pick_unique_name(fixture, "rec000", ".sub",
                                   out, sizeof(out)), 0);
    LS_EQ_STR(out, "rec000-1");

    /* The whole point: the pre-existing file is still on disk and its
       bytes are untouched.  If this ever fails the picker is silently
       destroying old captures. */
    LS_EQ_INT((int)fx_size("rec000"), 18);
    char buf[64];
    LS_CHECK(fx_read("rec000", buf, sizeof(buf)) > 0);
    LS_EQ_STR(buf, "old capture bytes\n");

    fx_rm_all();
}

LS_CASE(existing_capture_survives_a_save_attempt)
{
    /* Simulate what rec_save actually does end-to-end: pick a name, open it
       with the same atomic exclusive-create helper rec_save uses, write to the
       picked path.  The pre-existing rec000.sub must not be touched. */
    fx_setup();
    fx_write("rec000", "keep me\n");
    long old_sz = fx_size("rec000");
    LS_EQ_INT((int)old_sz, 8);

    char out[40] = {0};
    LS_EQ_INT(rec_pick_unique_name(fixture, "rec000", ".sub",
                                   out, sizeof(out)), 0);
    LS_CHECK(strcmp(out, "rec000") != 0);

    char path[160];
    fx_path(path, sizeof(path), out);
    FILE *f = rec_file_open_new(path);
    LS_CHECK(f != NULL);
    if (f) { fputs("new capture\n", f); fclose(f); }

    /* The bytes and size of the original are exactly as they were. */
    LS_EQ_INT((int)fx_size("rec000"), (int)old_sz);
    char buf[64];
    fx_read("rec000", buf, sizeof(buf));
    LS_EQ_STR(buf, "keep me\n");

    fx_rm_all();
}

LS_CASE(picker_walks_past_a_run_of_taken_suffixes)
{
    /* A busy directory: base and several suffixed variants are taken.  The
       picker must skip all of them and land on the first free slot rather
       than reusing any of the earlier ones. */
    fx_setup();
    fx_write("rec000",   "a");
    fx_write("rec000-1", "b");
    fx_write("rec000-2", "c");
    fx_write("rec000-3", "d");

    char out[40] = {0};
    LS_EQ_INT(rec_pick_unique_name(fixture, "rec000", ".sub",
                                   out, sizeof(out)), 0);
    LS_EQ_STR(out, "rec000-4");

    fx_rm_all();
}

LS_CASE(buffer_too_small_fails_without_writing)
{
    /* Contract check: on any failure the caller's out buffer must be left
       untouched, so a bad return value cannot be misread as a partially
       written name and reused against an existing capture. */
    fx_setup();
    fx_write("rec000", "x");

    char tiny[4] = { 'A', 'A', 'A', 0 };
    LS_EQ_INT(rec_pick_unique_name(fixture, "rec000", ".sub",
                                   tiny, sizeof(tiny)), -1);
    LS_EQ_INT(tiny[0], 'A');
    LS_EQ_INT(tiny[1], 'A');
    LS_EQ_INT(tiny[2], 'A');

    fx_rm_all();
}

LS_CASE(rejects_malformed_arguments)
{
    /* Guard against a mis-wired caller.  Every arg is validated so a
       future refactor cannot get a bad pointer past this and silently
       fall back to overwriting the old path. */
    char out[16];
    LS_EQ_INT(rec_pick_unique_name(NULL, "b", ".sub", out, sizeof(out)), -1);
    LS_EQ_INT(rec_pick_unique_name(".", NULL, ".sub", out, sizeof(out)), -1);
    LS_EQ_INT(rec_pick_unique_name(".", "b", NULL,   out, sizeof(out)), -1);
    LS_EQ_INT(rec_pick_unique_name(".", "b", ".sub", NULL, 16), -1);
    LS_EQ_INT(rec_pick_unique_name(".", "b", ".sub", out, 0),   -1);
    LS_EQ_INT(rec_pick_unique_name(".", "",  ".sub", out, sizeof(out)), -1);
}
