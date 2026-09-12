/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/ls_rlog.c */

#include "ls_test.h"
#include "ls_rlog.h"

#include <stdio.h>
#include <string.h>

typedef struct { uint32_t n; char tag[12]; } rec_t;

static const char *PATH = "test_rlog.bin";

static void scrub(void) { remove(PATH); }

static rec_t mk(uint32_t n)
{
    rec_t r;
    memset(&r, 0, sizeof(r));
    r.n = n;
    snprintf(r.tag, sizeof(r.tag), "r%u", (unsigned)n);
    return r;
}

/* ------------------------------------------------- the arithmetic alone -- */

LS_CASE(the_oldest_slot_never_underflows)
{

    LS_EQ_INT(0, (int)ls_rlog_slot_of(0, 24, 24, 0));
    LS_EQ_INT(1, (int)ls_rlog_slot_of(0, 24, 24, 1));
    LS_EQ_INT(23, (int)ls_rlog_slot_of(0, 24, 24, 23));

    /* A part-full ring: three records written, head at 3, oldest is slot 0. */
    LS_EQ_INT(0, (int)ls_rlog_slot_of(3, 3, 24, 0));
    LS_EQ_INT(2, (int)ls_rlog_slot_of(3, 3, 24, 2));

    /* Wrapped: head 5 on a full 24 means the oldest is slot 5. */
    LS_EQ_INT(5, (int)ls_rlog_slot_of(5, 24, 24, 0));
    LS_EQ_INT(4, (int)ls_rlog_slot_of(5, 24, 24, 23));
}

LS_CASE(every_slot_is_visited_exactly_once)
{
    /* The property that matters more than any single index: walking a full
       ring has to touch each slot once. A wrong formula that still produces
       in-range answers passes the cases above and fails this. */
    for (uint32_t head = 0; head < 8; head++) {
        int seen[8] = { 0 };
        for (uint32_t i = 0; i < 8; i++)
            seen[ls_rlog_slot_of(head, 8, 8, i)]++;
        for (int s = 0; s < 8; s++)
            LS_CHECK_MSG(seen[s] == 1,
                         "head %u: slot %d visited %d times",
                         (unsigned)head, s, seen[s]);
    }
}

LS_CASE(a_zero_capacity_ring_is_not_a_division_by_zero)
{
    LS_EQ_INT(0, (int)ls_rlog_slot_of(3, 3, 0, 1));
}

/* ------------------------------------------------------ through the file -- */

LS_CASE(records_come_back_oldest_first)
{
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 8));
    LS_EQ_INT(0, ls_rlog_count(&lg));

    for (uint32_t i = 0; i < 5; i++) {
        const rec_t r = mk(i);
        LS_CHECK(ls_rlog_append(&lg, &r));
    }
    LS_EQ_INT(5, ls_rlog_count(&lg));

    rec_t out[8];
    LS_EQ_INT(5, ls_rlog_read(&lg, out, 8));
    for (uint32_t i = 0; i < 5; i++)
        LS_EQ_INT((int)i, (int)out[i].n);

    ls_rlog_close(&lg);
    scrub();
}

LS_CASE(a_full_ring_drops_the_oldest_and_keeps_the_order)
{
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 4));

    for (uint32_t i = 0; i < 10; i++) {
        const rec_t r = mk(i);
        LS_CHECK(ls_rlog_append(&lg, &r));
    }
    LS_EQ_INT(4, ls_rlog_count(&lg));

    rec_t out[4];
    LS_EQ_INT(4, ls_rlog_read(&lg, out, 4));
    /* 6,7,8,9 - the last four, in the order they happened. */
    for (int i = 0; i < 4; i++)
        LS_EQ_INT(6 + i, (int)out[i].n);
    LS_CHECK(!strcmp(out[3].tag, "r9"));

    ls_rlog_close(&lg);
    scrub();
}

LS_CASE(asking_for_fewer_than_there_are_gives_the_NEWEST_ones)
{
    /* A chat wants the end of the conversation. Returning the first three of
       ten would be the wrong three and would look like the log had stopped
       updating. */
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 16));
    for (uint32_t i = 0; i < 10; i++) { const rec_t r = mk(i); ls_rlog_append(&lg, &r); }

    rec_t out[3];
    LS_EQ_INT(3, ls_rlog_read(&lg, out, 3));
    LS_EQ_INT(7, (int)out[0].n);
    LS_EQ_INT(8, (int)out[1].n);
    LS_EQ_INT(9, (int)out[2].n);

    ls_rlog_close(&lg);
    scrub();
}

LS_CASE(it_survives_being_closed_and_opened_again)
{
    /* The whole point. A reboot is a close and an open, and if the header
       does not come back the history is gone - which is the defect this was
       written for. */
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 8));
    for (uint32_t i = 0; i < 6; i++) { const rec_t r = mk(i); ls_rlog_append(&lg, &r); }
    ls_rlog_close(&lg);

    ls_rlog_t again;
    LS_CHECK(ls_rlog_open(&again, PATH, sizeof(rec_t), 8));
    LS_EQ_INT(6, ls_rlog_count(&again));

    rec_t out[8];
    LS_EQ_INT(6, ls_rlog_read(&again, out, 8));
    LS_EQ_INT(0, (int)out[0].n);
    LS_EQ_INT(5, (int)out[5].n);
    LS_CHECK(!strcmp(out[5].tag, "r5"));

    ls_rlog_close(&again);
    scrub();
}

LS_CASE(a_wrapped_ring_also_survives_a_reopen)
{
    /* Reopening a ring that has wrapped is the case where a head/count pair
       restored wrongly reads plausibly and in the wrong order. */
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 4));
    for (uint32_t i = 0; i < 7; i++) { const rec_t r = mk(i); ls_rlog_append(&lg, &r); }
    ls_rlog_close(&lg);

    ls_rlog_t again;
    LS_CHECK(ls_rlog_open(&again, PATH, sizeof(rec_t), 4));
    rec_t out[4];
    LS_EQ_INT(4, ls_rlog_read(&again, out, 4));
    for (int i = 0; i < 4; i++) LS_EQ_INT(3 + i, (int)out[i].n);

    ls_rlog_close(&again);
    scrub();
}

LS_CASE(a_file_with_different_geometry_is_replaced_not_reinterpreted)
{
    /* A firmware change that resizes the record must not read the old
       bytes as the new struct. Losing the history is the honest outcome;
       records that look real and are not is the dangerous one. */
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 8));
    for (uint32_t i = 0; i < 5; i++) { const rec_t r = mk(i); ls_rlog_append(&lg, &r); }
    ls_rlog_close(&lg);

    typedef struct { uint32_t a, b, c, d, e; } wider_t;
    ls_rlog_t other;
    LS_CHECK(ls_rlog_open(&other, PATH, sizeof(wider_t), 8));
    LS_EQ_INT(0, ls_rlog_count(&other));
    ls_rlog_close(&other);
    scrub();
}

LS_CASE(clear_empties_it_and_the_file_still_works)
{
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 8));
    for (uint32_t i = 0; i < 5; i++) { const rec_t r = mk(i); ls_rlog_append(&lg, &r); }
    LS_CHECK(ls_rlog_clear(&lg));
    LS_EQ_INT(0, ls_rlog_count(&lg));

    const rec_t r = mk(99);
    LS_CHECK(ls_rlog_append(&lg, &r));
    rec_t out[8];
    LS_EQ_INT(1, ls_rlog_read(&lg, out, 8));
    LS_EQ_INT(99, (int)out[0].n);

    ls_rlog_close(&lg);
    scrub();
}

LS_CASE(replace_writes_the_whole_ring_and_reads_back_in_order)
{
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 8));

    rec_t in[5];
    for (uint32_t i = 0; i < 5; i++) in[i] = mk(100 + i);
    LS_CHECK(ls_rlog_replace(&lg, in, 5));
    LS_EQ_INT(5, ls_rlog_count(&lg));

    rec_t out[8];
    LS_EQ_INT(5, ls_rlog_read(&lg, out, 8));
    for (int i = 0; i < 5; i++) LS_EQ_INT(100 + i, (int)out[i].n);

    /* Replacing with fewer must not leave the tail of the previous write
       readable - a history that grew back after being trimmed would be worse
       than one that lost entries. */
    rec_t two[2] = { mk(7), mk(8) };
    LS_CHECK(ls_rlog_replace(&lg, two, 2));
    LS_EQ_INT(2, ls_rlog_count(&lg));
    LS_EQ_INT(2, ls_rlog_read(&lg, out, 8));
    LS_EQ_INT(7, (int)out[0].n);
    LS_EQ_INT(8, (int)out[1].n);

    ls_rlog_close(&lg);
    scrub();
}

LS_CASE(a_replaced_ring_survives_a_reopen_and_a_full_one_wraps_to_zero)
{
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 4));

    rec_t full[4];
    for (uint32_t i = 0; i < 4; i++) full[i] = mk(i);
    LS_CHECK(ls_rlog_replace(&lg, full, 4));
    ls_rlog_close(&lg);

    ls_rlog_t again;
    LS_CHECK(ls_rlog_open(&again, PATH, sizeof(rec_t), 4));
    LS_EQ_INT(4, ls_rlog_count(&again));
    rec_t out[4];
    LS_EQ_INT(4, ls_rlog_read(&again, out, 4));
    for (int i = 0; i < 4; i++) LS_EQ_INT(i, (int)out[i].n);

    const rec_t more = mk(99);
    LS_CHECK(ls_rlog_append(&again, &more));
    LS_EQ_INT(4, ls_rlog_read(&again, out, 4));
    LS_EQ_INT(1, (int)out[0].n);
    LS_EQ_INT(99, (int)out[3].n);

    ls_rlog_close(&again);
    scrub();
}

LS_CASE(replace_refuses_more_than_it_can_hold)
{
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 4));
    rec_t many[6];
    for (uint32_t i = 0; i < 6; i++) many[i] = mk(i);
    LS_CHECK(!ls_rlog_replace(&lg, many, 6));
    /* Refused, not partially applied. */
    LS_EQ_INT(0, ls_rlog_count(&lg));
    ls_rlog_close(&lg);
    scrub();
}

LS_CASE(one_record_at_a_time_agrees_with_reading_the_lot)
{
    /* The whole point of read_at is that a caller with no buffer
       gets the same records in the same order as one with a big one. If the
       two ever disagree the wrong half is whichever the reader is not
       using, which is the kind of fault that shows up as a peer list that is
       subtly out of order. */
    scrub();
    ls_rlog_t lg;
    LS_CHECK(ls_rlog_open(&lg, PATH, sizeof(rec_t), 4));

    /* Six into four, so the ring has wrapped and the slot arithmetic is
       actually doing something. */
    for (uint32_t i = 0; i < 6; i++) { const rec_t r = mk(i); LS_CHECK(ls_rlog_append(&lg, &r)); }

    rec_t all[4];
    LS_EQ_INT(4, ls_rlog_read(&lg, all, 4));
    for (int i = 0; i < 4; i++) {
        rec_t one;
        LS_EQ_INT(1, ls_rlog_read_at(&lg, i, &one));
        LS_EQ_INT((int)all[i].n, (int)one.n);
    }

    /* Past the end is 0 and does not write to the output. */
    rec_t spare = mk(999);
    LS_EQ_INT(0, ls_rlog_read_at(&lg, 4, &spare));
    LS_EQ_INT(999, (int)spare.n);
    LS_EQ_INT(0, ls_rlog_read_at(&lg, -1, &spare));
    LS_EQ_INT(999, (int)spare.n);

    ls_rlog_close(&lg);
    scrub();
}

LS_CASE(no_card_is_a_no_op_rather_than_a_fault)
{
    /* A board with no card in it must keep working from RAM. Every call has
       to be safe on a log that never opened. */
    ls_rlog_t lg;
    LS_CHECK(!ls_rlog_open(&lg, "/no/such/dir/x.bin", sizeof(rec_t), 8));

    const rec_t r = mk(1);
    LS_CHECK(!ls_rlog_append(&lg, &r));
    rec_t out[4];
    LS_EQ_INT(0, ls_rlog_read(&lg, out, 4));
    LS_EQ_INT(0, ls_rlog_count(&lg));
    LS_CHECK(!ls_rlog_clear(&lg));
    LS_CHECK(!ls_rlog_replace(&lg, &r, 1));
    rec_t sink;
    LS_EQ_INT(0, ls_rlog_read_at(&lg, 0, &sink));
    ls_rlog_close(&lg);
}

LS_CASE(nonsense_arguments_are_refused)
{
    ls_rlog_t lg;
    LS_CHECK(!ls_rlog_open(&lg, PATH, 0, 8));
    LS_CHECK(!ls_rlog_open(&lg, PATH, sizeof(rec_t), 0));
    LS_CHECK(!ls_rlog_open(&lg, NULL, sizeof(rec_t), 8));
    LS_CHECK(!ls_rlog_open(NULL, PATH, sizeof(rec_t), 8));
    scrub();
}
