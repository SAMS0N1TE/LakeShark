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

static uint64_t card_total = 8000000000ULL, card_free = 7000000000ULL;
static bool card_ok = true;
static unsigned card_checks;
static char pruned_path[CALL_PATH_MAX];
bool call_archive_card_space(uint64_t *total, uint64_t *free)
{
    ++card_checks;
    if (pruned_path[0]) {
        struct stat st;
        LS_CHECK(stat(pruned_path, &st) != 0);
        card_free = 2000000000ULL;
    }
    *total = card_total; *free = card_free; return card_ok;
}
static bool saved_option(void *context, int option, int32_t *value)
{
    if (!context || option != CALL_OPT_DAYS) return false;
    *value = *(int32_t *)context; return true;
}

LS_CASE(receiver_policy_writer_and_browser_share_completed_calls)
{
    char root[128]; snprintf(root, sizeof(root), "call-archive-test-%lu", (unsigned long)getpid());
    LS_EQ_INT(make_dir(root), 0);
    call_archive_test_load_options(saved_option, NULL);
    LS_EQ_INT(call_archive_option(CALL_OPT_DAYS), 30);
    int32_t saved = 0;
    call_archive_test_load_options(saved_option, &saved);
    LS_EQ_INT(call_archive_option(CALL_OPT_DAYS), 0);
    call_archive_init();
    call_archive_set_option(CALL_OPT_MIN_MS, 0);
    call_archive_set_option(CALL_OPT_DAYS, 0);
    call_archive_set_option(CALL_OPT_P25, 1);
    call_archive_set_option(CALL_OPT_FM, 1);
    call_archive_set_option(CALL_OPT_FILTER, 1);
    LS_CHECK(call_archive_set_talkgroups("42, 43"));
    LS_CHECK(!call_archive_set_talkgroups("42, nope"));
    LS_CHECK(!call_archive_set_talkgroups("65536"));
    LS_CHECK(!call_archive_test_pump(root));
    unsigned initial_checks = card_checks;
    unsigned wakes = call_archive_test_wakes();
    for (int i = 0; i < 100; ++i) {
        call_archive_gate(CALL_P25, 851000000, false);
        call_archive_end(CALL_P25, false);
        call_archive_audio(CALL_P25_P2, 0, 0, 0, false, NULL, 0);
        LS_CHECK(!call_archive_test_pump(root));
    }
    LS_EQ_UINT(call_archive_test_wakes(), wakes);
    LS_EQ_UINT(card_checks, initial_checks);
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
    /* Older queued blocks must not turn quiet decoder polls into repeated
       notifications while the writer is still processing those blocks. */
    wakes = call_archive_test_wakes();
    for (int i = 0; i < 100; ++i) {
        call_archive_gate(CALL_P25, 851000000, false);
        call_archive_end(CALL_P25, false);
        call_archive_audio(CALL_P25, 0, 0, 0, false, NULL, 0);
    }
    LS_EQ_UINT(call_archive_test_wakes(), wakes);
    call_archive_end(CALL_P25, false);
    while (call_archive_test_pump(root)) {}
    LS_CHECK(card_checks > 0);
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
    /* Quiet P25 gates must not wake filesystem work on a healthy card,
       including the old one-second free-space deadline. */
    unsigned checks = card_checks;
    ls_test_sleep_ms(1100);
    for (int i = 0; i < 100; ++i) {
        call_archive_gate(CALL_P25, 851000000, false);
        call_archive_end(CALL_P25, false);
        LS_CHECK(!call_archive_test_pump(root));
    }
    LS_EQ_UINT(card_checks, checks);
    rmdir(dirs[0]); rmdir(dirs[1]); rmdir(root);
    call_archive_set_option(CALL_OPT_FILTER, 0);
}

LS_CASE(space_floor_closes_current_call_blocks_new_calls_and_resumes_after_pruning)
{
    char root[128]; snprintf(root, sizeof(root), "call-space-test-%lu", (unsigned long)getpid());
    LS_EQ_INT(make_dir(root), 0); call_archive_init();
    call_archive_set_option(CALL_OPT_FILTER, 0);
    call_archive_set_option(CALL_OPT_MIN_MS, 0);
    call_archive_set_option(CALL_OPT_DAYS, 0);
    card_total = 8000000000ULL; card_free = 1000000000ULL;
    call_archive_refresh(); call_archive_test_pump(root);
    call_archive_storage_t card; call_archive_storage(&card); LS_CHECK(!card.paused);
    int16_t pcm[160] = {1, 2, 3};
    call_archive_audio(CALL_FM, 155000000, 0, 0, true, pcm, 160);
    call_archive_test_pump(root); call_archive_test_pump(root);
    card_free = 999999999ULL;
    /* Cross the write budget without requesting a refresh. */
    for (int i = 0; i < 220; ++i) {
        call_archive_audio(CALL_FM, 155000000, 0, 0, true, pcm, 160);
        call_archive_test_pump(root);
    }
    while (call_archive_test_pump(root)) {}
    call_archive_storage(&card); LS_CHECK(card.paused); LS_EQ_INT(call_archive_count(), 1);
    call_entry_t entry; LS_CHECK(call_archive_entry(0, &entry));
    FILE *f = fopen(entry.path, "rb"); LS_CHECK(f);
    if (f) { uint8_t h[44]; LS_EQ_INT(fread(h, 1, 44, f), 44);
        uint32_t bytes = (uint32_t)h[40] | (uint32_t)h[41] << 8 |
            (uint32_t)h[42] << 16 | (uint32_t)h[43] << 24;
        LS_CHECK(bytes > 0 && bytes <= 65536 + sizeof(pcm));
        LS_EQ_INT(fseek(f, 0, SEEK_END), 0); LS_EQ_INT(ftell(f), bytes + 44); fclose(f); }
    call_archive_audio(CALL_FM, 155000000, 0, 0, true, pcm, 160);
    call_archive_end(CALL_FM, false);
    while (call_archive_test_pump(root)) {}
    LS_EQ_INT(call_archive_count(), 1);
    LS_CHECK(call_archive_delete(entry.path));
    card_free = 1000000000ULL;
    call_archive_test_pump(root); call_archive_storage(&card); LS_CHECK(!card.paused);
    char dir[CALL_PATH_MAX]; strcpy(dir, entry.path); *strrchr(dir, '/') = 0; rmdir(dir);
    /* The percentage floor wins on larger cards, including queued starts. */
    card_total = 64000000000ULL; card_free = 3199999999ULL;
    call_archive_audio(CALL_P25, 851000000, 42, 123, true, pcm, 160);
    call_archive_end(CALL_P25, false);
    while (call_archive_test_pump(root)) {}
    call_archive_storage(&card); LS_CHECK(card.paused); LS_EQ_INT(call_archive_count(), 0);
    card_free = 3200000000ULL; call_archive_refresh(); call_archive_test_pump(root);
    call_archive_storage(&card); LS_CHECK(!card.paused);
    call_archive_audio(CALL_P25, 851000000, 42, 123, true, pcm, 160);
    call_archive_end(CALL_P25, false); while (call_archive_test_pump(root)) {}
    LS_EQ_INT(call_archive_count(), 1); LS_CHECK(call_archive_entry(0, &entry));
    LS_CHECK(call_archive_delete(entry.path)); call_archive_test_pump(root);
    strcpy(dir, entry.path); *strrchr(dir, '/') = 0; rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/20240101", root); LS_EQ_INT(make_dir(dir), 0);
    snprintf(pruned_path, sizeof(pruned_path), "%s/20240101/000000_42.wav", root);
    f = fopen(pruned_path, "wb"); LS_CHECK(f); if (f) { fputs("old", f); fclose(f); }
    card_total = 8000000000ULL; card_free = 900000000ULL;
    call_archive_set_option(CALL_OPT_DAYS, 30);
    call_archive_audio(CALL_FM, 155000000, 0, 0, true, pcm, 160);
    call_archive_end(CALL_FM, false);
    while (call_archive_test_pump(root)) {}
    pruned_path[0] = 0;
    call_archive_storage(&card); LS_CHECK(!card.paused); LS_EQ_INT(call_archive_count(), 1);
    LS_CHECK(card.archive >= 364);
    LS_CHECK(call_archive_entry(0, &entry)); LS_CHECK(call_archive_delete(entry.path));
    call_archive_test_pump(root); strcpy(dir, entry.path); *strrchr(dir, '/') = 0; rmdir(dir);
    card_ok = false; call_archive_refresh(); call_archive_test_pump(root);
    call_archive_storage(&card); LS_CHECK(card.paused); LS_CHECK(!card.valid);
    card_ok = true; ls_test_sleep_ms(1100); call_archive_test_pump(root);
    call_archive_storage(&card); LS_CHECK(!card.paused); rmdir(root);
}

LS_CASE(local_offset_range_supports_quarter_hours_without_affecting_retention) {
    int days=call_archive_option(CALL_OPT_DAYS);
    call_archive_set_option(CALL_OPT_UTC_QUARTERS,-16);LS_EQ_INT(call_archive_option(CALL_OPT_UTC_QUARTERS),-16);
    call_archive_set_option(CALL_OPT_UTC_QUARTERS,23);LS_EQ_INT(call_archive_option(CALL_OPT_UTC_QUARTERS),23);
    call_archive_set_option(CALL_OPT_UTC_QUARTERS,57);LS_EQ_INT(call_archive_option(CALL_OPT_UTC_QUARTERS),23);
    call_archive_set_option(CALL_OPT_UTC_QUARTERS,-49);LS_EQ_INT(call_archive_option(CALL_OPT_UTC_QUARTERS),23);
    LS_EQ_INT(call_archive_option(CALL_OPT_DAYS),days);call_archive_set_option(CALL_OPT_UTC_QUARTERS,0);
}
