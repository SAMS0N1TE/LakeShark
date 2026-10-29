/* ls_decoded readers: a reader skipping a ring full of other kinds still finds
   the one it wants, in order, and counts what the ring overwrote. The reader
   now looks at a few entries per critical section; what it returns must not
   change. */
#include "ls_test.h"
#include "ls_decoded.h"

#include <string.h>

static void publish(uint32_t kind, uint32_t address)
{
    ls_decoded_t d;
    memset(&d, 0, sizeof(d));
    d.kind = kind;
    d.address = address;
    ls_decoded_publish(&d);
}

LS_CASE(reader_skips_other_kinds_across_many_entries_in_order)
{
    const int r = ls_decoded_open(LS_DECODED_PAGE);
    LS_CHECK(r >= 0);
    for (uint32_t k = 0; k < 200; k++) publish(LS_DECODED_ADSB, k);
    publish(LS_DECODED_PAGE, 1001);
    for (uint32_t k = 0; k < 37; k++) publish(LS_DECODED_ADSB, k);
    publish(LS_DECODED_PAGE, 1002);
    for (uint32_t k = 0; k < 3; k++) publish(LS_DECODED_ADSB, k);
    ls_decoded_t d;
    uint32_t lost = 0;
    LS_CHECK(ls_decoded_next(r, &d, &lost));
    LS_EQ_UINT(d.address, 1001);
    LS_CHECK(ls_decoded_next(r, &d, &lost));
    LS_EQ_UINT(d.address, 1002);
    LS_CHECK(!ls_decoded_next(r, &d, &lost));
    LS_EQ_UINT(lost, 0);
    ls_decoded_close(r);
}

LS_CASE(reader_left_behind_counts_the_overwritten_entries)
{
    const int r = ls_decoded_open(LS_DECODED_PAGE | LS_DECODED_ADSB);
    LS_CHECK(r >= 0);
    for (uint32_t k = 0; k < LS_DECODED_RING + 10; k++) publish(LS_DECODED_ADSB, k);
    ls_decoded_t d;
    uint32_t lost = 0;
    LS_CHECK(ls_decoded_next(r, &d, &lost));
    LS_EQ_UINT(lost, 10);
    LS_EQ_UINT(d.address, 10);
    unsigned n = 1;
    while (ls_decoded_next(r, &d, &lost)) n++;
    LS_EQ_UINT(n, LS_DECODED_RING);
    LS_EQ_UINT(d.address, LS_DECODED_RING + 9);
    ls_decoded_close(r);
    /* A closed reader gets nothing. */
    publish(LS_DECODED_PAGE, 7);
    LS_CHECK(!ls_decoded_next(r, &d, &lost));
}
