#include "ls_test.h"
#include "call_store.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0775)
#endif

static char root[128];
static call_block_t blocks[12];
static call_recorder_t r;
static call_writer_t writer;
static call_entry_t entries[12];
static unsigned sequence;
static void setup(void)
{
    snprintf(root, sizeof(root), "call-store-test-%lu-%u", (unsigned long)getpid(), sequence++);
    LS_EQ_INT(make_dir(root), 0);
    memset(&writer, 0, sizeof(writer)); call_recorder_init(&r, blocks, 12);
}
static void flush(void) { while (call_store_step(&writer, &r, root)) {} }
static void capture(int64_t time, unsigned tg, unsigned minimum_ms, bool abort)
{
    call_meta_t m = {.time = time, .hz = 851000000, .talkgroup = tg, .source = 123,
        .rate = 8000, .minimum_ms = minimum_ms, .gps = true, .lat = 42.1, .lon = -71.2};
    int16_t pcm[800]; for (int i = 0; i < 800; ++i) pcm[i] = (int16_t)(i - 400);
    LS_CHECK(call_recorder_begin(&r, &m, true));
    call_recorder_pcm(&r, pcm, 800); flush();
    /* Incomplete files are not browseable. */
    int before = call_store_scan(root, entries, 12);
    call_recorder_end(&r, abort); flush(); call_recorder_end(&r, false);
    LS_EQ_INT(call_store_scan(root, entries, 12), before + (!abort && minimum_ms <= 100));
}
static void cleanup(void)
{
    int n = call_store_scan(root, entries, 12);
    for (int i = 0; i < n; ++i) {
        LS_CHECK(call_store_delete(root, entries[i].path));
        char dir[CALL_PATH_MAX]; strcpy(dir, entries[i].path); *strrchr(dir, '/') = 0; rmdir(dir);
    }
    rmdir(root);
}
LS_CASE(finalization_preserves_pcm_metadata_and_resolves_same_second_collision)
{
    setup(); capture(1791374400, 42, 0, false); capture(1791374400, 42, 0, false);
    LS_EQ_INT(call_store_scan(root, entries, 12), 2);
    LS_CHECK(strcmp(entries[0].path, entries[1].path));
    LS_EQ_INT(entries[0].duration_ms, 100); LS_EQ_INT(entries[0].meta.talkgroup, 42);
    LS_EQ_INT(entries[0].meta.source, 123); LS_CHECK(entries[0].meta.gps);
    LS_NEAR(entries[0].meta.lat, 42.1, 0.0000001);
    FILE *f = fopen(entries[0].path, "rb"); LS_CHECK(f);
    uint8_t h[44]; LS_EQ_INT(fread(h, 1, 44, f), 44);
    LS_EQ_INT(h[40] | (h[41] << 8), 1600);
    int16_t pcm[800]; LS_EQ_INT(fread(pcm, 2, 800, f), 800);
    LS_EQ_INT(pcm[0], -400); LS_EQ_INT(pcm[799], 399); fclose(f);
    cleanup();
}
LS_CASE(encryption_and_minimum_length_remove_partial_files)
{
    setup(); capture(1791374400, 42, 0, true); capture(1791374400, 42, 101, false);
    LS_EQ_INT(call_store_scan(root, entries, 12), 0);
    char dir[CALL_PATH_MAX]; snprintf(dir, sizeof(dir), "%s/20261007", root); rmdir(dir); rmdir(root);
}
LS_CASE(retention_deletes_only_expired_valid_days_and_preserves_unknown_time)
{
    setup();
    int64_t today = 1791374400 / 86400;
    capture(1791374400, 42, 0, false);
    capture(1791374400 - 86400, 43, 0, false);
    capture(1791374400 - 7 * 86400, 44, 0, false);
    capture(0, 45, 0, false);
    char protected_file[CALL_PATH_MAX], invalid_dir[CALL_PATH_MAX];
    time_t old = 1791374400 - 7 * 86400; struct tm tm;
    LS_CHECK(call_utc_tm(old, &tm)); char day[16]; strftime(day, sizeof(day), "%Y%m%d", &tm);
    snprintf(protected_file, sizeof(protected_file), "%s/%s/notes.txt", root, day);
    FILE *f = fopen(protected_file, "w"); LS_CHECK(f); if (f) { fputs("keep", f); fclose(f); }
    snprintf(invalid_dir, sizeof(invalid_dir), "%s/20260231", root); LS_EQ_INT(make_dir(invalid_dir), 0);
    call_store_retain(root, today, 0); LS_EQ_INT(call_store_scan(root, entries, 12), 4);
    call_store_retain(root, today, 7); LS_EQ_INT(call_store_scan(root, entries, 12), 3);
    struct stat st; LS_EQ_INT(stat(protected_file, &st), 0); LS_EQ_INT(stat(invalid_dir, &st), 0);
    LS_EQ_INT(entries[0].meta.talkgroup, 42); LS_EQ_INT(entries[1].meta.talkgroup, 43);
    LS_EQ_INT(entries[2].meta.talkgroup, 45);
    LS_CHECK(!call_store_delete(root, "../outside.wav"));
    char unsafe[CALL_PATH_MAX]; snprintf(unsafe, sizeof(unsafe), "%s/20261007/../../outside.wav", root);
    LS_CHECK(!call_store_delete(root, unsafe));
    remove(protected_file); *strrchr(protected_file, '/') = 0; rmdir(protected_file); rmdir(invalid_dir);
    cleanup();
}
LS_CASE(sd_open_failure_is_counted_and_queue_still_drains)
{
    setup(); call_meta_t m = {.time = 1791374400, .rate = 8000};
    LS_CHECK(call_recorder_begin(&r, &m, true));
    LS_CHECK(call_store_step(&writer, &r, "not-a-parent/calls"));
    LS_EQ_INT(writer.errors, 1); LS_CHECK(!writer.file);
    call_recorder_end(&r, false); flush(); LS_CHECK(!call_recorder_peek(&r)); cleanup();
}

LS_CASE(protected_calls_survive_retention_and_can_be_unprotected) {
    setup();capture(1791374400-10*86400,42,0,false);
    LS_EQ_INT(call_store_scan(root,entries,12),1);LS_EQ_INT(entries[0].bytes,1644);
    char path[CALL_PATH_MAX];strcpy(path,entries[0].path);
    LS_CHECK(call_store_protect(root,path,true));call_store_retain(root,1791374400/86400,7);
    LS_EQ_INT(call_store_scan(root,entries,12),1);LS_CHECK(entries[0].kept);
    LS_CHECK(!call_store_protect(root,"../outside.wav",true));
    LS_CHECK(call_store_protect(root,path,false));call_store_retain(root,1791374400/86400,7);
    LS_EQ_INT(call_store_scan(root,entries,12),0);rmdir(root);
}

LS_CASE(playback_lease_defers_retention_and_explicit_delete_until_close)
{
    setup(); capture(1791374400 - 10 * 86400, 42, 0, false);
    LS_EQ_INT(call_store_scan(root, entries, 12), 1);
    char path[CALL_PATH_MAX]; strcpy(path, entries[0].path);
    FILE *file = call_store_playback_open(path); LS_CHECK(file);
    call_store_retain(root, 1791374400 / 86400, 7);
    LS_EQ_INT(call_store_scan(root, entries, 12), 1);
    LS_CHECK(!call_store_delete(root, path));
    call_store_playback_close(file);
    call_store_retain(root, 1791374400 / 86400, 7);
    LS_EQ_INT(call_store_scan(root, entries, 12), 0); rmdir(root);
}
