#include "ls_test.h"
#include "call_archive.h"
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0775)
#endif

static unsigned card_checks;
static uint64_t card_free = 900000000ULL;
bool call_archive_card_space(uint64_t *total, uint64_t *free)
{
    ++card_checks;
    *total = 8000000000ULL; *free = card_free;
    return true;
}

/* Separate process: no earlier case can populate the space cache. */
LS_CASE(quiet_start_blocks_until_work_and_first_recording_still_checks_space)
{
    char root[128]; snprintf(root, sizeof(root), "call-idle-test-%lu", (unsigned long)getpid());
    LS_EQ_INT(make_dir(root), 0);
    call_archive_init();
    LS_CHECK(!call_archive_test_pump(root));
    LS_EQ_UINT(card_checks, 0);
    unsigned wakes = call_archive_test_wakes();
    for (int i = 0; i < 100; ++i) {
        call_archive_gate(CALL_P25, 851000000, false);
        call_archive_end(CALL_P25, false);
        call_archive_gate(CALL_P25_P2, 851000000, false);
        LS_CHECK(!call_archive_test_pump(root));
    }
    LS_EQ_UINT(call_archive_test_wakes(), wakes);
    LS_EQ_UINT(card_checks, 0);

    /* Deferring the startup query must never bypass admission on a full card. */
    int16_t pcm[160] = {1, 2, 3};
    call_archive_audio(CALL_P25, 851000000, 42, 123, true, pcm, 160);
    call_archive_end(CALL_P25, false);
    while (call_archive_test_pump(root)) {}
    LS_EQ_UINT(card_checks, 1);
    call_archive_storage_t card; call_archive_storage(&card);
    LS_CHECK(card.valid && card.paused);
    LS_EQ_INT(call_archive_count(), 0);

    card_free = 2000000000ULL;
    call_archive_refresh();
    LS_CHECK(!call_archive_test_pump(root));
    call_archive_storage(&card); LS_CHECK(!card.paused);
    LS_EQ_UINT(card_checks, 2);
    rmdir(root);
}
