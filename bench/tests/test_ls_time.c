/* LS_TEST_SOURCES: */
/* Wall-clock rendering, on the host. */

#include "ls_test.h"
#include "ls_time.h"

#include <string.h>
#include <time.h>

/* 2024-01-01T00:00:00Z - the boundary between "we trust this" and "we
   ignore it as a stale reading" that ls_time.c pins in the source. */
#define AFTER_2024   1704067200LL
/* 2000-01-01T00:00:00Z - inside the "synced but implausible" band. */
#define STALE_2000    946684800LL

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* --- the "not synced" branch is honest ---------------------------------- */

LS_CASE(unsynced_prints_uptime_marker_not_a_date)
{
    /* real_time == 0 is the "no wall clock" state SNTP has not overwritten
       yet. Rendered output must never be mistaken for a calendar date. */
    char out[LS_TIME_STAMP_MAX];
    size_t n = ls_time_render_stamp_at(out, sizeof(out), 0, 42000000LL);
    LS_CHECK_MSG(n > 0, "renderer produced no output");

    /* The visible marker the task requires. */
    LS_CHECK_MSG(starts_with(out, "up "),
                 "unsynced stamp missing 'up ' prefix: [%s]", out);

    /* And absolutely nothing that reads like a real date. The four-digit
       year is the giveaway: a stamp starting with 19xx or 20xx would be
       parsed as a calendar year by every tool downstream. */
    LS_CHECK_MSG(!(out[0] >= '0' && out[0] <= '9'),
                 "unsynced stamp began with a digit: [%s]", out);
    LS_CHECK_MSG(strstr(out, "1970") == NULL,
                 "unsynced stamp shows 1970 (the epoch): [%s]", out);
    LS_CHECK_MSG(strstr(out, "2000") == NULL,
                 "unsynced stamp shows a plausible year: [%s]", out);
}

LS_CASE(unsynced_carries_actual_uptime_seconds)
{
    char out[LS_TIME_STAMP_MAX];
    ls_time_render_stamp_at(out, sizeof(out), 0, 42000000LL);
    /* 42 000 000 us = 42 s. The uptime marker exists so pages logged
       before sync can still be ordered within a boot. */
    LS_CHECK_MSG(strstr(out, "42") != NULL,
                 "uptime seconds missing from stamp: [%s]", out);
    LS_CHECK_MSG(strstr(out, "s") != NULL,
                 "unit missing from stamp: [%s]", out);
}

LS_CASE(unsynced_zero_uptime_is_zero_not_negative)
{
    char out[LS_TIME_STAMP_MAX];
    ls_time_render_stamp_at(out, sizeof(out), 0, 0);
    /* First 100 ms of boot must still print something, and it must not
       be negative. */
    LS_CHECK(strstr(out, "-") == NULL);
    LS_CHECK(starts_with(out, "up "));
}

/* --- the "synced" branch produces a real ISO-8601 UTC stamp ------------ */

LS_CASE(synced_prints_iso8601_utc)
{
    char out[LS_TIME_STAMP_MAX];
    /* 2024-01-01T00:00:00Z is 1704067200 - the boundary the module uses
       and an epoch you can verify by hand. The renderer must produce that
       exact ISO form so a filename or log line is grep-able. */
    time_t t = (time_t)1704067200LL;
    size_t n = ls_time_render_stamp_at(out, sizeof(out), t, 0);
    LS_CHECK(n > 0);
    LS_EQ_STR(out, "2024-01-01T00:00:00Z");
}

LS_CASE(synced_never_starts_with_up_prefix)
{
    /* The two forms must not be confusable: an uptime marker begins "up ",
       so the synced form must not. */
    char out[LS_TIME_STAMP_MAX];
    ls_time_render_stamp_at(out, sizeof(out), (time_t)AFTER_2024, 0);
    LS_CHECK_MSG(!starts_with(out, "up "),
                 "synced stamp reused the unsynced prefix: [%s]", out);
}

/* --- the "safety belt": a stale RTC reads early, we treat it as unsynced */

LS_CASE(implausibly_early_time_falls_back_to_uptime)
{
    /* Y2000 is well before the 2024 cutoff the module enforces. This is
       the wedged-RTC path: something has "set" the clock but it is not
       trustworthy, so we prefer the honest uptime. */
    char out[LS_TIME_STAMP_MAX];
    ls_time_render_stamp_at(out, sizeof(out), (time_t)STALE_2000, 99000000LL);
    LS_CHECK_MSG(starts_with(out, "up "),
                 "implausible time was rendered as real: [%s]", out);
    LS_CHECK_MSG(strstr(out, "2000") == NULL,
                 "stale year 2000 leaked into stamp: [%s]", out);
    LS_CHECK_MSG(strstr(out, "1970") == NULL,
                 "epoch year 1970 leaked into stamp: [%s]", out);
}

LS_CASE(unix_epoch_zero_is_treated_as_unset)
{
    /* time_t = 0 is 1970-01-01T00:00:00Z. Any renderer that prints that
       string is telling a lie every time a device boots without SNTP. */
    char out[LS_TIME_STAMP_MAX];
    ls_time_render_stamp_at(out, sizeof(out), (time_t)0, 5000000LL);
    LS_CHECK_MSG(strstr(out, "1970") == NULL,
                 "epoch-zero was rendered as a real date: [%s]", out);
}

LS_CASE(overshot_future_time_falls_back_too)
{
    /* 2101-01-01. Beyond the plausibility ceiling; a badly overshot NTP
       or a corrupt RTC gets the uptime marker, not a year 2101 stamp
       that would look "in the calendar" to a naive reader. */
    char out[LS_TIME_STAMP_MAX];
    ls_time_render_stamp_at(out, sizeof(out), (time_t)4133980800LL, 1000000LL);
    LS_CHECK_MSG(starts_with(out, "up "),
                 "future time was rendered as real: [%s]", out);
}

/* --- boundary handling: caller passing tiny buffers ---------------------- */

LS_CASE(zero_cap_writes_nothing)
{
    LS_EQ_UINT(ls_time_render_stamp_at(NULL, 0, 0, 0), 0);

    char sentinel[4] = { 'X', 'Y', 'Z', 0 };
    LS_EQ_UINT(ls_time_render_stamp_at(sentinel, 0, 0, 0), 0);
    LS_EQ_STR(sentinel, "XYZ");
}

LS_CASE(short_buffer_still_nul_terminates)
{
    /* A page log using a 12-char field must not walk off the end. */
    char out[12];
    memset(out, 0xAA, sizeof(out));
    (void)ls_time_render_stamp_at(out, sizeof(out), (time_t)AFTER_2024, 0);
    LS_CHECK(out[sizeof(out) - 1] == '\0');
}

/* --- the API the firmware actually uses --------------------------------- */

LS_CASE(is_synced_flag_flips_with_test_hook)
{
    /* The bench cannot bring up SNTP but must be able to prove that
       ls_time_is_synced is what gates the "real time" branch. */
    ls_time_test_set_synced(false);
    LS_CHECK(!ls_time_is_synced());

    ls_time_test_set_synced(true);
    LS_CHECK(ls_time_is_synced());

    /* And left flipped-back so later tests do not inherit state. */
    ls_time_test_set_synced(false);
}

/* --- capture filenames across the time-sync boundary ------------------- */

LS_CASE(capture_names_switch_from_uptime_to_wall_clock_after_sync)
{
    /* These are consecutive saves in one boot: the first happens
       before SNTP answers and the second immediately after. Pin both exact
       filename components so an uptime capture cannot masquerade as a dated
       one and the post-sync capture sorts on its real UTC time. */
    char before[64];
    char after[64];

    ls_time_render_filename_at(before, sizeof(before), "rec",
                               (time_t)0, 5972000000LL);
    ls_time_render_filename_at(after, sizeof(after), "rec",
                               (time_t)1704067200LL, 5973000000LL);

    LS_EQ_STR(before, "rec_up-5972s");
    LS_EQ_STR(after, "rec_2024-01-01T00-00-00Z");
    LS_CHECK_MSG(strstr(before, "_up-") != NULL,
                 "pre-sync capture lost its uptime marker: %s", before);
    LS_CHECK_MSG(strstr(after, "_up-") == NULL,
                 "post-sync capture still looks unsynced: %s", after);
}

LS_CASE(filename_stamp_live_path_observes_sync_transition)
{
    char before[64];
    char after[64];

    ls_time_test_set_synced(false);
    ls_time_render_filename(before, sizeof(before), "screen");
    ls_time_test_set_synced(true);
    ls_time_render_filename(after, sizeof(after), "screen");
    ls_time_test_set_synced(false);

    LS_CHECK_MSG(strncmp(before, "screen_up-", 10) == 0,
                 "live pre-sync name was not uptime: %s", before);
    LS_CHECK_MSG(strncmp(after, "screen_up-", 10) != 0,
                 "live post-sync name was not wall clock: %s", after);
    LS_CHECK_MSG(strchr(after, ':') == NULL && strchr(after, ' ') == NULL,
                 "wall-clock filename is not FAT-safe: %s", after);
}
