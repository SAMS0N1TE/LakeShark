/* LS_TEST_SOURCES: ${APP}/rec/rec_space.c */
/**/
/* Free-space check for the capture writer.  Before this fix rec_save()
   opened the .sub straight away and streamed edges at it, so a full SD
   or SPIFFS landed as a truncated file that still parsed the header and
   listed as a normal capture - the recorder had stopped recording without
   saying so.  The estimate, the check, the shortage message and the
   partial-file classifier live in their own C file so the bench can pin
   the arithmetic and the truncation contract without a filesystem, a
   screen or a radio; the wiring in app_rec.c is only a statvfs call, a
   `.part` rename and a listing filter. */

#include "ls_test.h"
#include "rec_space.h"

#include <string.h>

/* -------- estimate: covers the .sub header + edges + sidecar with slack -- */

LS_CASE(estimate_at_zero_edges_still_reserves_the_header_and_sidecar)
{
    /* A capture with no edges is degenerate (rec_save returns -1) but the
       estimate must not report 0 either - the header and sidecar still
       cost bytes, and a caller that treats "0 needed" as "always fits"
       would let a save through on a completely full disk. */
    LS_CHECK(rec_space_estimate_bytes(0) >= 1024);
}

LS_CASE(estimate_scales_linearly_with_edges)
{
    /* Doubling the edge count should roughly double the estimate - the
       per-edge budget is what dominates once the header is amortised.
       Pinning this catches an off-by-a-factor if someone quietly changes
       the RAW_Data wrap constant. */
    uint64_t small = rec_space_estimate_bytes(100);
    uint64_t big   = rec_space_estimate_bytes(4000);
    LS_CHECK(big > small);
    /* 40x edges is at least 20x more bytes (leaves plenty of room for
       the fixed header dominating the small case). */
    LS_CHECK(big >= small * 20);
}

LS_CASE(estimate_is_pessimistic_versus_a_real_capture)
{
    /* A real 350-edge .sub off this device measured about 3.6 KB with a
       full sidecar.  The estimate must be at least that big, ideally
       comfortably above, so the check does not admit a save that then
       runs out mid-write.  10 KB is the tolerance - well above measured,
       still small enough that a nominally-empty 8 MB SPIFFS accepts a
       normal capture. */
    uint64_t est = rec_space_estimate_bytes(350);
    LS_CHECK(est >= 3600);
    LS_CHECK(est <= 10000);
}

LS_CASE(estimate_at_max_edges_stays_under_a_full_spiffs_partition)
{
    /* REC_MAX_EDGES is 4096.  A worst-case capture at that count must
       still fit inside a bare 8 MB SPIFFS with room for filesystem
       overhead - otherwise the check refuses every save even on an
       empty partition and the recorder is worse off than before. */
    uint64_t est = rec_space_estimate_bytes(4096);
    LS_CHECK(est < 6ull * 1024ull * 1024ull);
}

LS_CASE(estimate_treats_negative_edges_as_zero)
{
    /* Defensive: rec_save is the only caller and it early-returns on
       s_edges <= 0, but the pure function must not overflow or produce
       a nonsensical huge number if a future caller passes -1. */
    LS_EQ_UINT(rec_space_estimate_bytes(-1),
               rec_space_estimate_bytes(0));
}

/* -------- rec_space_ok: pure comparison the caller acts on ---------------- */

LS_CASE(space_ok_when_available_matches_needed_exactly)
{
    /* Boundary: available == needed must be OK.  If it were <, then the
       last byte on a filesystem could never be spent even by a save
       whose estimate was correct to the byte - and the estimate is
       already pessimistic. */
    LS_CHECK(rec_space_ok(1024, 1024));
}

LS_CASE(space_ok_when_available_exceeds_needed)
{
    LS_CHECK(rec_space_ok(1024, 8ull * 1024ull * 1024ull));
}

LS_CASE(space_not_ok_when_available_is_short)
{
    LS_CHECK(!rec_space_ok(2048, 1024));
}

LS_CASE(space_not_ok_when_available_is_zero)
{
    /* The full-disk case.  rec_save must refuse before opening the
       file - the whole point of the fix. */
    LS_CHECK(!rec_space_ok(rec_space_estimate_bytes(350), 0));
}

LS_CASE(space_ok_does_not_underflow_on_large_available)
{
    /* 4 GB SD cards report a byte count that overflows int32 by a lot.
       The pure function uses uint64 on both sides so a large available
       is still a large available - not something that wrapped negative
       and became "insufficient". */
    uint64_t big = (uint64_t)1 << 33;   /* 8 GB */
    LS_CHECK(rec_space_ok(1024, big));
}

/* -------- format_shortage: names both numbers so the user knows -------- */

LS_CASE(shortage_message_names_the_gap)
{
    /* The task's done-when: a refused capture must name the space
       needed and available so the user knows whether to delete one
       file or reformat the card. */
    char b[96];
    int w = rec_space_format_shortage(b, sizeof(b), 50000, 1234);
    LS_CHECK(w > 0);
    LS_CHECK(strstr(b, "50000") != NULL);
    LS_CHECK(strstr(b, "1234")  != NULL);
    LS_CHECK(strstr(b, "free")  != NULL);
}

LS_CASE(shortage_message_rejects_null_output_buffer)
{

    LS_EQ_INT(rec_space_format_shortage(NULL, 32, 1, 2), 0);
    char b[8];
    LS_EQ_INT(rec_space_format_shortage(b, 0, 1, 2), 0);
}

LS_CASE(shortage_message_survives_a_short_buffer_without_writing_past_it)
{
    /* snprintf semantics: the caller may pass a tight buffer and the
       formatter must not write past it.  The buffer stays NUL-terminated
       at len-1 and the return value is the would-be length. */
    char b[8];
    memset(b, 0x5A, sizeof(b));
    int w = rec_space_format_shortage(b, sizeof(b), 100000, 42);
    LS_CHECK(w > 0);
    LS_CHECK(b[sizeof(b) - 1] == '\0');
}

/* -------- format_free: short human-readable free-space label ----------- */

LS_CASE(free_format_uses_bytes_for_small_values)
{
    /* Under 1 KiB stays in bytes so the reading isn't lied to by
       rounding.  482 B is not "0.5 KB" on any user's chart. */
    char b[32];
    rec_space_format_free(b, sizeof(b), 482);
    LS_EQ_STR(b, "482 B");
}

LS_CASE(free_format_uses_kb_for_kilobyte_range)
{
    char b[32];
    rec_space_format_free(b, sizeof(b), 12u * 1024u + 300u);
    /* 12.2 KB - rounded down.  300/1024 = 0.29, one decimal = 2. */
    LS_EQ_STR(b, "12.2 KB");
}

LS_CASE(free_format_uses_mb_for_megabyte_range)
{
    char b[32];
    rec_space_format_free(b, sizeof(b), 5ull * 1024ull * 1024ull + 512ull * 1024ull);
    /* 5.5 MB. */
    LS_EQ_STR(b, "5.5 MB");
}

LS_CASE(free_format_uses_gb_for_gigabyte_range)
{
    char b[32];
    rec_space_format_free(b, sizeof(b),
                          2ull * 1024ull * 1024ull * 1024ull +
                          512ull * 1024ull * 1024ull);
    /* 2.5 GB - the sort of number a 4 GB SD reports when half full. */
    LS_EQ_STR(b, "2.5 GB");
}

LS_CASE(free_format_never_rounds_up)
{
    /* Truncation contract: the printed number is never more than the
       actual free space, so a user watching the label empty out is not
       lied to when the card is about to be full.  1023 bytes shown as
       "1.0 KB" would be that lie. */
    char b[32];
    rec_space_format_free(b, sizeof(b), 1023);
    LS_EQ_STR(b, "1023 B");
}

LS_CASE(free_format_handles_zero_bytes)
{

    char b[32];
    rec_space_format_free(b, sizeof(b), 0);
    LS_EQ_STR(b, "0 B");
}

LS_CASE(free_format_rejects_null_output_buffer)
{
    LS_EQ_INT(rec_space_format_free(NULL, 32, 1024), 0);
    char b[8];
    LS_EQ_INT(rec_space_format_free(b, 0, 1024), 0);
}

/* -------- partial-file classifier: half-written captures are visible ---- */

LS_CASE(finished_capture_is_not_partial)
{
    /* A `.sub` written cleanly is what the FILES tab must be free to
       list and rec_load must be free to read - the classifier must
       not flag it. */
    LS_CHECK(!rec_capture_name_is_partial("rec003.sub"));
}

LS_CASE(part_extension_marks_a_partial_capture)
{
    /* rec_save writes to `<name>.sub.part` first and renames on close.
       A leftover `.part` is evidence the rename never happened - crash,
       power loss, out-of-space midway through.  The task's done-when:
       a half-written capture is not listed as a good one. */
    LS_CHECK(rec_capture_name_is_partial("rec003.sub.part"));
}

LS_CASE(bare_part_name_still_flagged)
{
    /* Screenshot.c may pick a name without the intermediate `.sub`.
       The classifier must catch every filename whose tail is `.part`,
       not just the specific two-extension case. */
    LS_CHECK(rec_capture_name_is_partial("screen.part"));
}

LS_CASE(part_prefix_does_not_falsely_flag)
{
    /* "part" in the middle of a name is not a partial marker - only
       the tail is.  Otherwise a caller could name their capture
       "party.sub" and it would silently disappear from the list. */
    LS_CHECK(!rec_capture_name_is_partial("party.sub"));
}

LS_CASE(null_and_empty_are_not_partial)
{
    /* Defensive: readdir cannot legitimately hand back either, but if
       it does the classifier must not flag them as a failed write. */
    LS_CHECK(!rec_capture_name_is_partial(NULL));
    LS_CHECK(!rec_capture_name_is_partial(""));
}

LS_CASE(short_name_shorter_than_the_extension_is_not_partial)
{
    /* Boundary: a name whose length is less than strlen(".part") must
       not be tail-compared with a read past its start. */
    LS_CHECK(!rec_capture_name_is_partial("x"));
    LS_CHECK(!rec_capture_name_is_partial(".par"));
}
