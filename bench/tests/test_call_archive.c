#include "ls_test.h"
#include "call_archive.h"
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0775)
#endif

LS_CASE(receiver_policy_writer_and_browser_share_completed_calls)
{
    char root[128]; snprintf(root, sizeof(root), "call-archive-test-%lu", (unsigned long)getpid());
    LS_EQ_INT(make_dir(root), 0);
    call_archive_init();
    call_archive_set_option(CALL_OPT_MIN_MS, 0);
    call_archive_set_option(CALL_OPT_DAYS, 0);
    call_archive_set_option(CALL_OPT_P25, 1);
    call_archive_set_option(CALL_OPT_FM, 1);
    call_archive_set_option(CALL_OPT_FILTER, 1);
    LS_CHECK(call_archive_set_talkgroups("42, 43"));
    LS_CHECK(!call_archive_set_talkgroups("42, nope"));
    LS_CHECK(!call_archive_set_talkgroups("65536"));
    int16_t pcm[160] = {1, 2, 3};
    call_archive_audio(CALL_P25, 851000000, 99, 123, true, pcm, 160);
    call_archive_audio(CALL_P25, 851000000, 42, 123, false, pcm, 160);
    call_archive_end(CALL_P25, false);
    while (call_archive_test_pump(root)) {}
    LS_EQ_INT(call_archive_count(), 0);
    call_archive_audio(CALL_P25, 851000000, 42, 123, true, pcm, 160);
    call_archive_audio(CALL_FM, 155000000, 0, 0, true, pcm, 160);
    LS_CHECK(call_archive_busy());
    call_archive_end(CALL_P25, false); call_archive_end(CALL_FM, false);
    while (call_archive_test_pump(root)) {}
    LS_EQ_INT(call_archive_count(), 2);
    LS_CHECK(!call_archive_busy());
    call_entry_t entry;
    char dirs[2][CALL_PATH_MAX];
    for (int i = 0; i < 2; ++i) {
        LS_CHECK(call_archive_entry(i, &entry));
        LS_EQ_INT(entry.duration_ms, entry.meta.rate == 8000 ? 20 : 10);
        LS_CHECK(entry.meta.hz == 851000000 || entry.meta.hz == 155000000);
        strcpy(dirs[i], entry.path); *strrchr(dirs[i], '/') = 0;
    }
    for (int i = 0; i < 2; ++i) {
        LS_CHECK(call_archive_entry(0, &entry));
        LS_CHECK(call_archive_delete(entry.path));
        /* Only one filesystem command is pending at once. */
        LS_CHECK(!call_archive_delete(entry.path));
        while (call_archive_test_pump(root)) {}
    }
    LS_EQ_INT(call_archive_count(), 0);
    LS_EQ_INT(call_archive_errors(), 0);
    rmdir(dirs[0]); rmdir(dirs[1]); rmdir(root);
    call_archive_set_option(CALL_OPT_FILTER, 0);
}
