#include "ls_test.h"
#include "p25_lr_wide.h"
#include <string.h>

/* The wide sink: the shared LR session's small 125k calls regrouped into
   P25's 320-pair queue blocks, numbered without gaps from each open. */

static p25_lr_wide_sink_t k;
static void feed_pairs(unsigned pairs, unsigned *counter)
{
    uint8_t d[1024];
    LS_CHECK(pairs * 4 <= sizeof(d));
    for (unsigned i = 0; i < pairs; ++i) {
        const unsigned v = (*counter)++;
        d[4 * i] = (uint8_t)v; d[4 * i + 1] = (uint8_t)(v >> 8);
        d[4 * i + 2] = (uint8_t)(v >> 16); d[4 * i + 3] = 0x5A;
    }
    p25_lr_wide_sink_consume(&k, d, pairs * 4, true);
}
static unsigned pair_value(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16; }

LS_CASE(small_calls_regroup_into_whole_blocks_in_order)
{
    memset(&k, 0, sizeof(k));
    LS_CHECK(p25_lr_wide_sink_open(&k, 7));
    unsigned counter = 0;
    /* One wide slot gives ~111 pairs; a backlog gives up to 256. */
    const unsigned sizes[] = {111, 112, 111, 256, 111, 3, 97};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) feed_pairs(sizes[i], &counter);
    /* 801 pairs: two whole blocks out, 161 staged. */
    uint64_t expected = 0; unsigned blocks = 0, value = 0;
    const lr2021_iq_block *b;
    while ((b = lr2021_iq_queue_peek(&k.queue))) {
        LS_EQ_UINT(LR2021_IQ_BLOCK_BYTES, b->bytes);
        LS_EQ_UINT(7, b->generation);
        LS_EQ_UINT(expected, b->first_pair);
        for (size_t p = 0; p < b->bytes / 4; ++p) LS_EQ_UINT(value++, pair_value(b->data + 4 * p));
        expected += b->bytes / 4; ++blocks;
        lr2021_iq_queue_release(&k.queue);
    }
    LS_EQ_UINT(2, blocks); LS_EQ_UINT(161 * 4, k.staged);
    LS_CHECK(!atomic_load(&k.broken));
}

LS_CASE(a_stream_end_while_open_breaks_the_run_and_closed_drops)
{
    memset(&k, 0, sizeof(k));
    LS_CHECK(p25_lr_wide_sink_open(&k, 1));
    unsigned counter = 0;
    feed_pairs(200, &counter);
    p25_lr_wide_sink_consume(&k, NULL, 0, false);
    LS_CHECK(atomic_load(&k.broken)); LS_EQ_UINT(1, atomic_load(&k.resets));
    LS_EQ_UINT(0, k.staged);
    /* Broken: nothing more is queued until P25 opens a new generation. */
    feed_pairs(256, &counter); feed_pairs(256, &counter);
    LS_CHECK(!lr2021_iq_queue_peek(&k.queue));
    /* A retune: P25 closes first, so the old run's end is not a break. */
    p25_lr_wide_sink_close(&k);
    feed_pairs(256, &counter);
    p25_lr_wide_sink_consume(&k, NULL, 0, false);
    LS_EQ_UINT(2, atomic_load(&k.resets));
    LS_CHECK(p25_lr_wide_sink_open(&k, 2));
    LS_CHECK(!atomic_load(&k.broken));
    counter = 0;
    feed_pairs(256, &counter); feed_pairs(256, &counter);
    const lr2021_iq_block *b = lr2021_iq_queue_peek(&k.queue);
    LS_CHECK(b); LS_EQ_UINT(2, b->generation); LS_EQ_UINT(0, b->first_pair);
    LS_EQ_UINT(0, pair_value(b->data));
}

LS_CASE(a_full_queue_breaks_instead_of_skipping)
{
    memset(&k, 0, sizeof(k));
    LS_CHECK(p25_lr_wide_sink_open(&k, 3));
    unsigned counter = 0;
    for (unsigned i = 0; i < 5 * LR2021_IQ_QUEUE_SLOTS; ++i) feed_pairs(256, &counter);
    LS_CHECK(atomic_load(&k.broken));
    uint64_t expected = 0; unsigned blocks = 0;
    const lr2021_iq_block *b;
    while ((b = lr2021_iq_queue_peek(&k.queue))) {
        LS_EQ_UINT(expected, b->first_pair); expected += b->bytes / 4; ++blocks;
        lr2021_iq_queue_release(&k.queue);
    }
    LS_EQ_UINT(LR2021_IQ_QUEUE_SLOTS, blocks);
}

LS_CASE(preference_follows_the_shared_setting_unless_pinned)
{
    /* Boot: narrow, whatever the shared setting says. */
    LS_EQ_INT(P25_LR_WIDE_OFF, p25_lr_wide_pref());
    LS_CHECK(!p25_lr_wide_pick(p25_lr_wide_pref(), true));
    LS_CHECK(!p25_lr_wide_pick(P25_LR_WIDE_FOLLOW, false));
    LS_CHECK(p25_lr_wide_pick(P25_LR_WIDE_FOLLOW, true));
    LS_CHECK(p25_lr_wide_pick(P25_LR_WIDE_ON, false));
    LS_CHECK(!p25_lr_wide_pick(P25_LR_WIDE_OFF, true));
    p25_lr_wide_set_pref(P25_LR_WIDE_FOLLOW); LS_EQ_INT(P25_LR_WIDE_FOLLOW, p25_lr_wide_pref());
    p25_lr_wide_set_pref((p25_lr_wide_pref_t)9); LS_EQ_INT(P25_LR_WIDE_OFF, p25_lr_wide_pref());
    LS_EQ_STR("follow", p25_lr_wide_pref_name(P25_LR_WIDE_FOLLOW));
}
