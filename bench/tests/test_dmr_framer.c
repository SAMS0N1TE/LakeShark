/* LS_TEST_SOURCES: ${APP}/dmr/dmr_sync.c ${APP}/dmr/dmr_slot.c
 *                  ${APP}/dmr/dmr_bptc.c ${APP}/dmr/dmr_lc.c
 *                  ${APP}/dmr/dmr_burst.c ${APP}/dmr/dmr_framer.c
 *                  fixtures/dmr_gen.c
 *
 * DMR burst framer - the step between a symbol stream and the decoder that
 * was already tested in test_dmr.c. Everything above the framer works on a
 * burst that starts in the right place; this is what decides where that is.
 * Voice audio remains out of scope. */

#include "ls_test.h"

#include "dmr.h"
#include "dmr_gen.h"

#include <string.h>

/* ETSI TS 102 361-1 section 9.1.1, the same four the detector matches. */
static const uint8_t BS_VOICE[6] = { 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 };
static const uint8_t BS_DATA [6] = { 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D };
static const uint8_t MS_VOICE[6] = { 0x7F, 0x7D, 0x5D, 0xD5, 0x7D, 0xFD };
static const uint8_t MS_DATA [6] = { 0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57 };

#define MAX_ERR 5

static int burst_bit(const uint8_t *b, unsigned i)
{
    return (b[i >> 3] >> (7u - (i & 7u))) & 1u;
}

static dmr_lc_t sample_lc(void)
{
    dmr_lc_t lc;
    memset(&lc, 0, sizeof(lc));
    lc.protect_flag    = 0;
    lc.flco            = 0;          /* group voice channel user */
    lc.fid             = 0;
    lc.service_options = 0;
    lc.destination     = 31337;      /* talkgroup */
    lc.source          = 2420001;    /* radio ID  */
    return lc;
}

/* A deterministic bit source for the lead-in. Nothing here needs randomness
 * to be good, only for it to be the same every run. */
static uint32_t rnd_state = 0x1234567u;
static int rnd_bit(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (int)((rnd_state >> 16) & 1u);
}

/* Feed a whole burst and return how many times the framer reported one. */
static int feed_burst(dmr_framer_t *f, const uint8_t *burst,
                      dmr_burst_frame_t *last)
{
    int hits = 0;
    for (unsigned i = 0; i < DMR_BURST_BITS; i++) {
        dmr_burst_frame_t out;
        if (dmr_framer_bit(f, burst_bit(burst, i), &out)) {
            hits++;
            if (last) *last = out;
        }
    }
    return hits;
}

LS_CASE(a_clean_burst_is_reported_once_and_handed_back_whole)
{
    uint8_t burst[33];
    const dmr_lc_t lc = sample_lc();
    dmr_gen_burst(burst, BS_VOICE, 7, 1, &lc);

    dmr_framer_t f;
    dmr_framer_reset(&f, MAX_ERR);
    dmr_burst_frame_t got;
    memset(&got, 0, sizeof(got));

    LS_EQ_INT(feed_burst(&f, burst, &got), 1);
    LS_EQ_INT((int)got.class_id, (int)DMR_SYNC_BS_VOICE);
    LS_EQ_UINT(got.sync_errors, 0);
    LS_CHECK(memcmp(got.burst, burst, 33) == 0);
}

LS_CASE(the_burst_is_still_found_after_arbitrary_leading_noise)
{
    uint8_t burst[33];
    const dmr_lc_t lc = sample_lc();
    dmr_gen_burst(burst, BS_DATA, 3, 3, &lc);

    /* Every lead-in length modulo 8 - a bit-shifted window must work as well
     * as a byte-aligned one, which is the whole reason the framer shifts. */
    for (unsigned lead = 0; lead < 40; lead++) {
        dmr_framer_t f;
        dmr_framer_reset(&f, MAX_ERR);
        dmr_burst_frame_t out;
        rnd_state = 0x1234567u + lead;

        int early = 0;
        for (unsigned i = 0; i < lead; i++)
            if (dmr_framer_bit(&f, rnd_bit(), &out)) early++;
        LS_EQ_INT(early, 0);

        dmr_burst_frame_t got;
        memset(&got, 0, sizeof(got));
        LS_EQ_INT(feed_burst(&f, burst, &got), 1);
        LS_EQ_INT((int)got.class_id, (int)DMR_SYNC_BS_DATA);
        LS_CHECK(memcmp(got.burst, burst, 33) == 0);
    }
}

LS_CASE(a_sync_pattern_anywhere_but_the_burst_centre_is_not_a_burst)
{
    /* The same 48 bits that would lock at offset 108, put at offset 0. A
     * framer that hunted for sync anywhere instead of at its fixed place
     * inside the burst would report this. */
    uint8_t block[33];
    memset(block, 0, sizeof(block));
    for (unsigned i = 0; i < DMR_SYNC_BITS; i++) {
        const int bit = (BS_VOICE[i / 8] >> (7 - (i % 8))) & 1;
        if (bit) block[i >> 3] |= (uint8_t)(1u << (7u - (i & 7u)));
    }

    dmr_framer_t f;
    dmr_framer_reset(&f, MAX_ERR);
    dmr_burst_frame_t out;
    int hits = feed_burst(&f, block, &out);

    /* And keep going well past it, so the pattern is walked through every
     * window position including 108. */
    for (unsigned i = 0; i < 4u * DMR_BURST_BITS; i++)
        if (dmr_framer_bit(&f, 0, &out)) hits++;

    LS_EQ_INT(hits, 0);
}

LS_CASE(two_back_to_back_bursts_are_both_reported)
{
    uint8_t first[33], second[33];
    const dmr_lc_t lc = sample_lc();
    dmr_gen_burst(first,  BS_VOICE, 1, 1, &lc);
    dmr_gen_burst(second, BS_DATA,  1, 3, &lc);

    dmr_framer_t f;
    dmr_framer_reset(&f, MAX_ERR);

    dmr_burst_frame_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    LS_EQ_INT(feed_burst(&f, first,  &a), 1);
    LS_EQ_INT(feed_burst(&f, second, &b), 1);

    LS_EQ_INT((int)a.class_id, (int)DMR_SYNC_BS_VOICE);
    LS_EQ_INT((int)b.class_id, (int)DMR_SYNC_BS_DATA);
    LS_CHECK(memcmp(a.burst, first,  33) == 0);
    LS_CHECK(memcmp(b.burst, second, 33) == 0);
}

LS_CASE(every_sync_class_survives_the_framer)
{
    const struct { const uint8_t *pattern; dmr_sync_class_t id; uint8_t dt; }
    cases[] = {
        { BS_VOICE, DMR_SYNC_BS_VOICE, 1 },
        { BS_DATA,  DMR_SYNC_BS_DATA,  3 },
        { MS_VOICE, DMR_SYNC_MS_VOICE, 1 },
        { MS_DATA,  DMR_SYNC_MS_DATA,  3 },
    };
    const dmr_lc_t lc = sample_lc();

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t burst[33];
        dmr_gen_burst(burst, cases[i].pattern, 9, cases[i].dt, &lc);

        dmr_framer_t f;
        dmr_framer_reset(&f, MAX_ERR);
        dmr_burst_frame_t got;
        memset(&got, 0, sizeof(got));

        LS_EQ_INT(feed_burst(&f, burst, &got), 1);
        LS_EQ_INT((int)got.class_id, (int)cases[i].id);
    }
}

LS_CASE(sync_damage_locks_up_to_the_threshold_and_not_past_it)
{
    const dmr_lc_t lc = sample_lc();

    for (unsigned errors = 0; errors <= MAX_ERR + 2u; errors++) {
        uint8_t burst[33];
        dmr_gen_burst(burst, BS_VOICE, 5, 1, &lc);

        /* Flip `errors` bits spread across the sync field itself. */
        for (unsigned e = 0; e < errors; e++) {
            const unsigned idx = DMR_SYNC_OFFSET_BITS + e * 7u;
            burst[idx >> 3] ^= (uint8_t)(1u << (7u - (idx & 7u)));
        }

        dmr_framer_t f;
        dmr_framer_reset(&f, MAX_ERR);
        dmr_burst_frame_t got;
        memset(&got, 0, sizeof(got));
        const int hits = feed_burst(&f, burst, &got);

        if (errors <= MAX_ERR) {
            LS_CHECK_MSG(hits == 1, "sync within threshold did not lock");
            LS_EQ_UINT(got.sync_errors, errors);
        } else {
            LS_CHECK_MSG(hits == 0, "sync past threshold locked anyway");
        }
    }
}

LS_CASE(a_framed_burst_still_yields_its_colour_code_and_link_control)
{
    /* The point of the framer: what comes out of it is what the rest of the
     * DMR path already knows how to read. Nothing here re-implements that -
     * it hands the framed burst straight to the tested decoders. */
    const dmr_lc_t sent = sample_lc();
    uint8_t burst[33];
    dmr_gen_burst(burst, BS_VOICE, 11, 1, &sent);

    dmr_framer_t f;
    dmr_framer_reset(&f, MAX_ERR);
    dmr_burst_frame_t got;
    memset(&got, 0, sizeof(got));
    LS_EQ_INT(feed_burst(&f, burst, &got), 1);

    LS_EQ_UINT(dmr_burst_colour_code(got.burst), 11u);

    uint8_t coded[DMR_BPTC_BITS / 8 + 1];
    uint8_t decoded[12];
    dmr_burst_extract_bptc(got.burst, coded);
    LS_CHECK(dmr_bptc_decode(coded, decoded) >= 0);

    /* parse, not decode. dmr_gen_lc_bits zeroes the 24 RS parity bits, so a
     * fixture burst cannot satisfy dmr_lc_decode's parity check - which is
     * correct of it, and is why test_dmr.c only calls decode against the
     * MMDVM reference vectors. The framer is what is under test here, not
     * the RS check. */
    dmr_lc_t out;
    memset(&out, 0, sizeof(out));
    dmr_lc_parse(decoded, &out);
    LS_EQ_UINT(out.destination, sent.destination);
    LS_EQ_UINT(out.source, sent.source);
    LS_EQ_UINT(out.flco, sent.flco);
}

LS_CASE(a_burst_arriving_one_slot_at_a_time_drives_the_two_slot_tracker)
{
    /* Framer to tracker, which is the seam the task called the real work.
     * The slot number is the caller's: the framer does not invent one. */
    const dmr_lc_t lc = sample_lc();
    uint8_t voice[33], data[33];
    dmr_gen_burst(voice, BS_VOICE, 2, 1, &lc);
    dmr_gen_burst(data,  BS_DATA,  2, 3, &lc);

    dmr_framer_t f;
    dmr_framer_reset(&f, MAX_ERR);
    dmr_tracker_t t;
    dmr_tracker_reset(&t);

    for (int round = 0; round < 3; round++) {
        dmr_burst_frame_t got;
        memset(&got, 0, sizeof(got));
        LS_EQ_INT(feed_burst(&f, voice, &got), 1);
        LS_EQ_INT(dmr_tracker_burst(&t, 1, got.class_id), 1);

        memset(&got, 0, sizeof(got));
        LS_EQ_INT(feed_burst(&f, data, &got), 1);
        LS_EQ_INT(dmr_tracker_burst(&t, 2, got.class_id), 1);
    }

    LS_EQ_UINT(t.slot[0].bursts, 3u);
    LS_EQ_UINT(t.slot[1].bursts, 3u);
    LS_EQ_INT((int)t.slot[0].state, (int)DMR_SLOT_VOICE);
    LS_EQ_INT((int)t.slot[1].state, (int)DMR_SLOT_DATA);
    LS_EQ_UINT(t.slot[0].total_losses, 0u);
    LS_EQ_UINT(t.slot[1].total_losses, 0u);
}
