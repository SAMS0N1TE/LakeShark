#include "ls_test.h"
#include "scan_journal_codec.h"
#include <string.h>

LS_CASE(scan_journal_formats_versioned_validity_and_escaped_text)
{
    scan_journal_record_t r = {0};
    r.kind = SCAN_JOURNAL_HOLD;
    r.session_id = 3;
    r.sequence = 7;
    r.uptime_ms = 1234;
    r.unix_time = 1700000000;
    r.mode = 1;
    r.power_pct = 42;
    r.requested_hz = 154785000;
    r.effective_hz = 154784900;
    r.effective_known = true;
    r.gps_valid = false;
    snprintf(r.channel, sizeof(r.channel), "A\"B\\C");
    snprintf(r.reason, sizeof(r.reason), "carrier\nheld");
    char line[512];
    LS_CHECK(scan_journal_format_json(&r, line, sizeof(line)) > 0);
    LS_CHECK(strstr(line, "\"schema\":1") != NULL);
    LS_CHECK(strstr(line, "\"event\":\"hold\"") != NULL);
    LS_CHECK(strstr(line, "\"session_id\":3") != NULL);
    LS_CHECK(strstr(line, "A\\\"B\\\\C") != NULL);
    LS_CHECK(strstr(line, "carrier\\nheld") != NULL);
    LS_CHECK(strstr(line, "\"gps_valid\":false") != NULL);
    LS_CHECK(strstr(line, "\"latitude\":null") != NULL);
    LS_CHECK(strstr(line, "\"effective_known\":true") != NULL);
}

LS_CASE(scan_journal_rejects_truncated_records)
{
    scan_journal_record_t r = {.kind = SCAN_JOURNAL_SUMMARY, .power_pct = -1};
    char tiny[24];
    LS_EQ_INT(scan_journal_format_json(&r, tiny, sizeof(tiny)), -1);
    LS_EQ_INT(tiny[0], 0);
}

LS_CASE(scan_journal_policy_handles_no_card_full_card_and_partial_write)
{
    scan_journal_policy_t p;
    scan_journal_policy_begin(&p, false, 0);
    LS_CHECK(!scan_journal_policy_can_write(&p, 100));

    scan_journal_policy_begin(&p, true, SCAN_JOURNAL_FREE_RESERVE);
    LS_CHECK(!scan_journal_policy_can_write(&p, 1));

    scan_journal_policy_begin(&p, true, SCAN_JOURNAL_FREE_RESERVE + 4096);
    LS_CHECK(scan_journal_policy_can_write(&p, 512));
    scan_journal_policy_note_write(&p, 512, 100);
    LS_CHECK(!p.storage_ok);
    LS_EQ_INT(p.write_errors, 1);
}

LS_CASE(scan_journal_policy_bounds_growth_preserves_drop_count_and_restarts)
{
    scan_journal_policy_t p;
    scan_journal_policy_begin(&p, true,
                              SCAN_JOURNAL_FREE_RESERVE + SCAN_JOURNAL_FILE_LIMIT + 4096);
    p.bytes = SCAN_JOURNAL_FILE_LIMIT - 10;
    LS_CHECK(!scan_journal_policy_can_write(&p, 11));
    LS_CHECK(scan_journal_policy_can_write(&p, 10));
    scan_journal_policy_note_drop(&p, 3);
    LS_EQ_INT(p.queue_dropped, 3);
    scan_journal_policy_stop(&p);
    LS_CHECK(!scan_journal_policy_can_write(&p, 1));

    scan_journal_policy_begin(&p, true, SCAN_JOURNAL_FREE_RESERVE + 1024);
    LS_CHECK(p.active && p.storage_ok);
    LS_EQ_INT(p.bytes, 0);
    LS_EQ_INT(p.queue_dropped, 0);
}
