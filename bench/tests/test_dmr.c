/* LS_TEST_SOURCES: ${APP}/dmr/dmr_sync.c ${APP}/dmr/dmr_slot.c
 *                  ${APP}/dmr/dmr_bptc.c ${APP}/dmr/dmr_lc.c
 *                  ${APP}/dmr/dmr_burst.c
 *                  fixtures/dmr_gen.c
 *
 * DMR Tier II - sync class detection, two-slot TDMA state machine, BPTC(196,96)
 * deinterleave/correction, and Voice LC Header field parse. Voice audio is
 * out of scope for this task; see the commit body. */

#include "ls_test.h"

#include "dmr.h"
#include "dmr_gen.h"
#include "dmr_reference_vectors.h"

#include <string.h>

LS_CASE(slot_type_matches_all_256_independent_mmdvm_codewords)
{
    for (unsigned i=0;i<256;i++) {
        uint32_t word=dmr_reference_slot[i];
        LS_EQ_UINT(dmr_slot_type_encode((uint8_t)i),word);
        uint8_t wire[3]={(uint8_t)(word>>12),(uint8_t)(word>>4),(uint8_t)(word<<4)};
        uint8_t cc=255,dt=255;
        LS_EQ_INT(dmr_slot_type_decode(wire,&cc,&dt),0);
        LS_EQ_UINT(cc,i>>4);
        LS_EQ_UINT(dt,i&15);
    }
}

LS_CASE(bptc_decodes_independent_mmdvm_lc_headers_and_terminators)
{
    for(unsigned i=0;i<sizeof(dmr_reference_lc)/sizeof(dmr_reference_lc[0]);i++) {
        uint8_t coded[25],decoded[12],encoded[25];
        dmr_burst_extract_bptc(dmr_reference_lc[i].burst,coded);
        LS_EQ_INT(dmr_bptc_decode(coded,decoded),0);
        LS_CHECK(memcmp(decoded,dmr_reference_lc[i].lc,12)==0);
        dmr_bptc_encode(dmr_reference_lc[i].lc,encoded);
        LS_CHECK(memcmp(encoded,coded,25)==0);
        for(unsigned bit=0;bit<196;bit++) {
            uint8_t damaged[25];memcpy(damaged,coded,25);
            damaged[bit/8]^=(uint8_t)(1u<<(7-bit%8));
            LS_CHECK(dmr_bptc_decode(damaged,decoded)>=0);
            LS_CHECK(memcmp(decoded,dmr_reference_lc[i].lc,12)==0);
        }
    }
}

LS_CASE(lc_requires_independent_masked_rs_parity_before_publishing_identity)
{
    for(unsigned i=0;i<sizeof(dmr_reference_lc)/sizeof(dmr_reference_lc[0]);i++) {
        const uint8_t *wire=dmr_reference_lc[i].lc;
        uint8_t type=(uint8_t)(1+i%2);
        dmr_lc_t decoded;
        LS_EQ_INT(dmr_lc_decode(wire,type,&decoded),1);
        LS_EQ_UINT(decoded.destination,((uint32_t)wire[3]<<16)|((uint32_t)wire[4]<<8)|wire[5]);
        LS_EQ_UINT(decoded.source,((uint32_t)wire[6]<<16)|((uint32_t)wire[7]<<8)|wire[8]);
        LS_EQ_UINT(decoded.protect_flag,wire[0]>>7);
        LS_EQ_UINT(decoded.flco,wire[0]&0x3f);
        LS_EQ_UINT(decoded.fid,wire[1]);
        LS_EQ_UINT(decoded.service_options,wire[2]);
        dmr_lc_t preserved;memset(&preserved,0xa5,sizeof(preserved));
        for(unsigned bit=0;bit<96;bit++) {
            uint8_t damaged[12];memcpy(damaged,wire,12);
            damaged[bit/8]^=(uint8_t)(1u<<(7-bit%8));
            decoded=preserved;
            LS_EQ_INT(dmr_lc_decode(damaged,type,&decoded),0);
            LS_CHECK(memcmp(&decoded,&preserved,sizeof(decoded))==0);
        }
        LS_EQ_INT(dmr_lc_decode(wire,(uint8_t)(3-type),&decoded),0);
        LS_EQ_INT(dmr_lc_decode(wire,3,&decoded),0);
        LS_EQ_INT(dmr_lc_decode(NULL,type,&decoded),0);
        LS_EQ_INT(dmr_lc_decode(wire,type,NULL),0);
    }
}

static const uint8_t k_bs_voice[6] = { 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 };
static const uint8_t k_bs_data [6] = { 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D };
static const uint8_t k_ms_voice[6] = { 0x7F, 0x7D, 0x5D, 0xD5, 0x7D, 0xFD };
static const uint8_t k_ms_data [6] = { 0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57 };

static void flip_bit(uint8_t *buf, unsigned bit_idx)
{
    buf[bit_idx >> 3] ^= (uint8_t)(1u << (7u - (bit_idx & 7u)));
}

/* --------------------------------------------------------------------------
 * sync detection
 * ------------------------------------------------------------------------ */

LS_CASE(sync_detect_distinguishes_all_four_etsi_classes)
{
    dmr_sync_match_t m;

    m = dmr_sync_detect(k_bs_voice, 5);
    LS_EQ_INT(m.class_id, DMR_SYNC_BS_VOICE);
    LS_EQ_INT(m.errors, 0);

    m = dmr_sync_detect(k_bs_data, 5);
    LS_EQ_INT(m.class_id, DMR_SYNC_BS_DATA);
    LS_EQ_INT(m.errors, 0);

    m = dmr_sync_detect(k_ms_voice, 5);
    LS_EQ_INT(m.class_id, DMR_SYNC_MS_VOICE);
    LS_EQ_INT(m.errors, 0);

    m = dmr_sync_detect(k_ms_data, 5);
    LS_EQ_INT(m.class_id, DMR_SYNC_MS_DATA);
    LS_EQ_INT(m.errors, 0);
}

LS_CASE(sync_detect_tolerates_bit_flips_up_to_threshold)
{
    uint8_t noisy[6];

    memcpy(noisy, k_bs_voice, 6);
    flip_bit(noisy, 0);
    flip_bit(noisy, 17);
    flip_bit(noisy, 34);
    dmr_sync_match_t m = dmr_sync_detect(noisy, 5);
    LS_EQ_INT(m.class_id, DMR_SYNC_BS_VOICE);
    LS_EQ_INT(m.errors, 3);
}

LS_CASE(sync_detect_rejects_random_bits_as_none)
{
    /* Random-ish garbage should sit well away from any pattern. */
    static const uint8_t noise[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    dmr_sync_match_t m = dmr_sync_detect(noise, 5);
    LS_EQ_INT(m.class_id, DMR_SYNC_NONE);
}

/* --------------------------------------------------------------------------
 * two-slot TDMA state machine
 * ------------------------------------------------------------------------ */

LS_CASE(slot_tracker_promotes_voice_and_data_independently_per_slot)
{
    dmr_tracker_t t;
    dmr_tracker_reset(&t);

    LS_EQ_INT(dmr_tracker_burst(&t, 1, DMR_SYNC_BS_VOICE), 1);
    LS_EQ_INT(dmr_tracker_burst(&t, 2, DMR_SYNC_BS_DATA), 1);

    LS_EQ_INT(t.slot[0].state, DMR_SLOT_VOICE);
    LS_EQ_INT(t.slot[1].state, DMR_SLOT_DATA);
    LS_EQ_UINT(t.slot[0].bursts, 1);
    LS_EQ_UINT(t.slot[1].bursts, 1);
}

LS_CASE(slot_tracker_survives_lost_burst_in_one_slot_while_other_continues)
{
    dmr_tracker_t t;
    dmr_tracker_reset(&t);

    /* Slot 1 carries a voice call; slot 2 carries a parallel data call. */
    dmr_tracker_burst(&t, 1, DMR_SYNC_BS_VOICE);
    dmr_tracker_burst(&t, 2, DMR_SYNC_BS_DATA);
    dmr_tracker_burst(&t, 1, DMR_SYNC_BS_VOICE);
    dmr_tracker_burst(&t, 2, DMR_SYNC_BS_DATA);

    /* Slot 1 loses a burst - the SLOT 2 side must be untouched. */
    dmr_tracker_burst(&t, 1, DMR_SYNC_NONE);
    LS_EQ_INT(t.slot[0].state, DMR_SLOT_VOICE);
    LS_EQ_UINT(t.slot[0].losses, 1);
    LS_EQ_UINT(t.slot[0].total_losses, 1);
    LS_EQ_INT(t.slot[1].state, DMR_SLOT_DATA);
    LS_EQ_UINT(t.slot[1].losses, 0);
    LS_EQ_UINT(t.slot[1].total_losses, 0);

    /* Slot 2 continues to receive normally. */
    dmr_tracker_burst(&t, 2, DMR_SYNC_BS_DATA);
    LS_EQ_UINT(t.slot[1].bursts, 3);

    /* Slot 1 recovers on the next burst - the transient loss did not reset
     * its state. */
    dmr_tracker_burst(&t, 1, DMR_SYNC_BS_VOICE);
    LS_EQ_INT(t.slot[0].state, DMR_SLOT_VOICE);
    LS_EQ_UINT(t.slot[0].losses, 0);
    LS_EQ_UINT(t.slot[0].total_losses, 1);
    LS_EQ_UINT(t.slot[0].bursts, 3);
}

LS_CASE(slot_tracker_rejects_invalid_slot_indices)
{
    dmr_tracker_t t;
    dmr_tracker_reset(&t);
    LS_EQ_INT(dmr_tracker_burst(&t, 0, DMR_SYNC_BS_VOICE), 0);
    LS_EQ_INT(dmr_tracker_burst(&t, 3, DMR_SYNC_BS_VOICE), 0);
    LS_EQ_INT(t.slot[0].bursts, 0);
    LS_EQ_INT(t.slot[1].bursts, 0);
}

/* --------------------------------------------------------------------------
 * BPTC(196,96)
 * ------------------------------------------------------------------------ */

LS_CASE(bptc_encode_decode_roundtrips_random_payloads)
{
    ls_rng_t rng;
    ls_rng_seed(&rng, 0xB27C0001u);

    for (int iter = 0; iter < 32; iter++) {
        uint8_t data[12];
        uint8_t coded[DMR_BPTC_BITS / 8 + 1];
        uint8_t out[12];

        for (int i = 0; i < 12; i++)
            data[i] = (uint8_t)(ls_rng_u32(&rng) & 0xffu);

        dmr_bptc_encode(data, coded);
        int errs = dmr_bptc_decode(coded, out);
        LS_EQ_INT(errs, 0);
        LS_CHECK_MSG(memcmp(data, out, 12) == 0,
                     "iter=%d roundtrip mismatch", iter);
    }
}

LS_CASE(bptc_corrects_one_bit_error_in_the_interleaved_wire)
{
    uint8_t data[12] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,
        0xCD, 0xEF, 0x10, 0x32, 0x54, 0x76,
    };
    uint8_t coded[DMR_BPTC_BITS / 8 + 1];
    uint8_t out[12];

    dmr_bptc_encode(data, coded);

    /* Flip a single bit at a data-carrying position on the wire. */
    flip_bit(coded, 42);

    int errs = dmr_bptc_decode(coded, out);
    LS_CHECK_MSG(errs >= 1, "expected at least one correction, got %d", errs);
    LS_CHECK_MSG(memcmp(data, out, 12) == 0, "one-bit error not corrected");
}

LS_CASE(bptc_corrects_isolated_errors_across_multiple_rows_and_columns)
{
    /* Two errors that land in different rows AND different columns of the
     * de-interleaved matrix are each single-bit errors in their own row
     * codeword and can be corrected by the Hamming(15,11,3) row pass.
     * The exact wire positions were chosen to satisfy that after the
     * (a*181)%196 permutation. */
    uint8_t data[12] = {
        0xF0, 0x0F, 0xA5, 0x5A, 0xC3, 0x3C,
        0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE,
    };
    uint8_t coded[DMR_BPTC_BITS / 8 + 1];
    uint8_t out[12];

    dmr_bptc_encode(data, coded);

    /* Two wire bits well apart (interleaved back into unrelated matrix
     * cells) demonstrate that row + column passes together handle multiple
     * errors that a single-row Hamming (15,11,3) alone could not. */
    flip_bit(coded, 10);
    flip_bit(coded, 100);

    int errs = dmr_bptc_decode(coded, out);
    LS_CHECK_MSG(errs >= 2, "expected >= 2 corrections, got %d", errs);
    LS_CHECK_MSG(memcmp(data, out, 12) == 0,
                 "two-error correction failed");
}

LS_CASE(bptc_correction_ceiling_records_recovery_limit)
{
    /* Sweep the correction ceiling across many random error patterns and
     * record how many bit-flips the decoder can survive.  This test does
     * not claim BPTC(196,96) corrects an arbitrary N-bit pattern - the
     * result is data-dependent - it pins the empirical ceiling so a
     * regression in the row/column passes is loud. */
    ls_rng_t rng;
    ls_rng_seed(&rng, 0xB27C0002u);
    uint8_t base[12];
    for (int i = 0; i < 12; i++)
        base[i] = (uint8_t)(ls_rng_u32(&rng) & 0xffu);

    uint8_t clean[DMR_BPTC_BITS / 8 + 1];
    dmr_bptc_encode(base, clean);

    int one_bit_recovered = 0;
    int two_bit_recovered = 0;
    int three_bit_recovered = 0;
    const int trials = 64;

    for (int trial = 0; trial < trials; trial++) {
        uint8_t noisy[DMR_BPTC_BITS / 8 + 1];
        uint8_t out[12];

        /* One-bit ceiling: 196 possible single-bit errors.  All must recover. */
        memcpy(noisy, clean, sizeof(noisy));
        unsigned p1 = ls_rng_u32(&rng) % 196u;
        flip_bit(noisy, p1);
        if (dmr_bptc_decode(noisy, out) >= 0 && memcmp(out, base, 12) == 0)
            one_bit_recovered++;

        /* Two-bit sweep.  BPTC typically recovers unless both errors fall in
         * the same row AND same column of the deinterleaved matrix. */
        memcpy(noisy, clean, sizeof(noisy));
        unsigned q1 = ls_rng_u32(&rng) % 196u;
        unsigned q2 = ls_rng_u32(&rng) % 196u;
        while (q2 == q1) q2 = ls_rng_u32(&rng) % 196u;
        flip_bit(noisy, q1);
        flip_bit(noisy, q2);
        if (dmr_bptc_decode(noisy, out) >= 0 && memcmp(out, base, 12) == 0)
            two_bit_recovered++;

        /* Three-bit sweep - not guaranteed by (196,96) but often recovered
         * when the errors scatter across the matrix. */
        memcpy(noisy, clean, sizeof(noisy));
        unsigned r1 = ls_rng_u32(&rng) % 196u;
        unsigned r2 = ls_rng_u32(&rng) % 196u;
        unsigned r3 = ls_rng_u32(&rng) % 196u;
        while (r2 == r1) r2 = ls_rng_u32(&rng) % 196u;
        while (r3 == r1 || r3 == r2) r3 = ls_rng_u32(&rng) % 196u;
        flip_bit(noisy, r1);
        flip_bit(noisy, r2);
        flip_bit(noisy, r3);
        if (dmr_bptc_decode(noisy, out) >= 0 && memcmp(out, base, 12) == 0)
            three_bit_recovered++;
    }

    /* All 1-bit patterns must recover, no exceptions. */
    LS_EQ_INT(one_bit_recovered, trials);

    /* Two-bit recovery must be the common case; log the empirical number
     * so a regression in the second row pass shows up. */
    LS_CHECK_MSG(two_bit_recovered >= trials * 8 / 10,
                 "2-bit recovery rate below floor: %d / %d",
                 two_bit_recovered, trials);
    ls_note("BPTC empirical recovery: 1-bit %d/%d  2-bit %d/%d  3-bit %d/%d",
            one_bit_recovered, trials,
            two_bit_recovered, trials,
            three_bit_recovered, trials);
}

/* --------------------------------------------------------------------------
 * Voice LC Header parse (colour code + TG + source)
 * ------------------------------------------------------------------------ */

LS_CASE(voice_lc_header_fields_come_out_of_the_burst_as_transmitted)
{
    /* Group Voice Channel User (FLCO = 0), FID = 0 (standard), TG 3100
     * (a well-known amateur DMR TG), source 1234567 (a valid CCS7 ID). */
    dmr_lc_t tx = {
        .protect_flag    = 0,
        .flco            = 0,
        .fid             = 0,
        .service_options = 0x20,
        .destination     = 3100u,
        .source          = 1234567u,
    };

    uint8_t burst[33];
    dmr_gen_burst(burst, k_bs_voice, /* colour code */ 7,
                  /* data type */ 0x1 /* voice LC header */, &tx);

    /* Extract the unaligned 48-bit sync from bit offset 108 into a local
     * MSB-first buffer so the detector can be re-run over the emitted burst. */
    uint8_t sync[6] = { 0 };
    for (unsigned i = 0; i < 48; i++) {
        int b = (burst[(108u + i) / 8] >> (7u - ((108u + i) & 7u))) & 1;
        if (b) sync[i / 8] |= (uint8_t)(1u << (7u - (i & 7u)));
    }
    dmr_sync_match_t sm = dmr_sync_detect(sync, 5);
    LS_EQ_INT(sm.class_id, DMR_SYNC_BS_VOICE);
    LS_EQ_INT(sm.errors, 0);

    /* Colour code comes out of the slot type field. */
    LS_EQ_UINT(dmr_burst_colour_code(burst), 7);

    /* Pull the BPTC(196,96) block out of info1|info2 and decode it. */
    uint8_t block[DMR_BPTC_BITS / 8 + 1];
    dmr_burst_extract_bptc(burst, block);
    uint8_t lc_bits[12];
    int errs = dmr_bptc_decode(block, lc_bits);
    LS_EQ_INT(errs, 0);

    dmr_lc_t rx;
    dmr_lc_parse(lc_bits, &rx);
    LS_EQ_UINT(rx.protect_flag,   0);
    LS_EQ_UINT(rx.flco,           0);
    LS_EQ_UINT(rx.fid,            0);
    LS_EQ_UINT(rx.service_options, 0x20);
    LS_EQ_UINT(rx.destination,    3100u);
    LS_EQ_UINT(rx.source,         1234567u);
}

LS_CASE(voice_lc_header_survives_one_bit_flip_in_the_bptc_payload)
{
    dmr_lc_t tx = {
        .protect_flag    = 0,
        .flco            = 3,          /* Unit-to-Unit Voice Channel User */
        .fid             = 0,
        .service_options = 0,
        .destination     = 9998u,
        .source          = 3141592u,
    };

    uint8_t burst[33];
    dmr_gen_burst(burst, k_ms_voice, 11, 0x1, &tx);

    /* Corrupt one bit inside info1. */
    flip_bit(burst, 30);

    uint8_t block[DMR_BPTC_BITS / 8 + 1];
    dmr_burst_extract_bptc(burst, block);
    uint8_t lc_bits[12];
    int errs = dmr_bptc_decode(block, lc_bits);
    LS_CHECK_MSG(errs >= 1, "expected at least one BPTC correction, got %d",
                 errs);

    dmr_lc_t rx;
    dmr_lc_parse(lc_bits, &rx);
    LS_EQ_UINT(rx.flco,        3);
    LS_EQ_UINT(rx.destination, 9998u);
    LS_EQ_UINT(rx.source,      3141592u);
    LS_EQ_UINT(dmr_burst_colour_code(burst), 11);
}

/* --------------------------------------------------------------------------
 * Slot Type FEC - Golay(20,8,7) per ETSI TS 102 361-1 §B.3.4
 * ------------------------------------------------------------------------ */

/* The 20-bit Slot Type codeword straddles the sync: burst positions
 * 98..107 hold the first 10 bits, 156..165 hold the last 10. */
static const unsigned k_slot_type_bits[20] = {
     98,  99, 100, 101, 102, 103, 104, 105, 106, 107,
    156, 157, 158, 159, 160, 161, 162, 163, 164, 165,
};

LS_CASE(slot_type_golay_2087_encoder_is_systematic_and_min_distance_seven)
{
    /* Systematic: bits 19..12 of the codeword are the data byte unchanged. */
    for (unsigned d = 0; d < 256; d++) {
        uint32_t cw = dmr_slot_type_encode((uint8_t)d);
        LS_EQ_UINT((cw >> 12) & 0xffu, d);
        LS_CHECK_MSG((cw & ~0xFFFFFu) == 0,
                     "codeword wider than 20 bits for data 0x%02x", d);
    }

    /* Any two distinct codewords must differ in at least 7 bit positions. */
    for (unsigned a = 0; a < 256; a++) {
        uint32_t cwa = dmr_slot_type_encode((uint8_t)a);
        for (unsigned b = a + 1; b < 256; b++) {
            uint32_t diff = cwa ^ dmr_slot_type_encode((uint8_t)b);
            int hd = __builtin_popcount(diff);
            LS_CHECK_MSG(hd >= 7,
                         "d(cw[%u], cw[%u]) = %d, expected >= 7", a, b, hd);
        }
    }
}

LS_CASE(slot_type_decode_recovers_from_one_two_and_three_bit_errors)
{
    ls_rng_t rng;
    ls_rng_seed(&rng, 0x2087C0DEu);

    dmr_lc_t tx = {
        .protect_flag    = 0,
        .flco            = 0,
        .fid             = 0,
        .service_options = 0,
        .destination     = 3100u,
        .source          = 1234567u,
    };

    /* Sweep every 1-, 2- and 3-bit combination that lands inside the
     * 20-bit slot-type region.  Golay(20,8,7) is guaranteed to correct
     * up to 3 errors, so every one of these must recover the true CC
     * and DT. */
    for (unsigned n_errs = 1; n_errs <= 3; n_errs++) {
        for (int trial = 0; trial < 40; trial++) {
            uint8_t burst[33];
            dmr_gen_burst(burst, k_bs_data, /* CC */ 9,
                          /* DT (data header) */ 0x6, &tx);

            /* Pick n_errs distinct positions from the 20-bit region. */
            unsigned picked[3] = {0};
            unsigned n_picked = 0;
            while (n_picked < n_errs) {
                unsigned idx = ls_rng_u32(&rng) % 20u;
                int dup = 0;
                for (unsigned i = 0; i < n_picked; i++)
                    if (picked[i] == idx) { dup = 1; break; }
                if (!dup) picked[n_picked++] = idx;
            }
            for (unsigned i = 0; i < n_picked; i++)
                flip_bit(burst, k_slot_type_bits[picked[i]]);

            /* Repack the slot-type field into the 3-byte helper input. */
            uint8_t packed[3] = { 0, 0, 0 };
            for (unsigned i = 0; i < 10; i++) {
                int b = (burst[(98u + i) / 8] >> (7u - ((98u + i) & 7u))) & 1;
                if (b) packed[i / 8] |= (uint8_t)(1u << (7u - (i & 7u)));
            }
            for (unsigned i = 0; i < 10; i++) {
                int b = (burst[(156u + i) / 8] >> (7u - ((156u + i) & 7u))) & 1;
                unsigned bit = 10u + i;
                if (b) packed[bit / 8] |= (uint8_t)(1u << (7u - (bit & 7u)));
            }

            uint8_t cc = 0xff, dt = 0xff;
            int corrected = dmr_slot_type_decode(packed, &cc, &dt);
            LS_CHECK_MSG(corrected == (int)n_errs,
                         "n_errs=%u trial=%d: corrected=%d cc=%u dt=%u",
                         n_errs, trial, corrected, cc, dt);
            LS_EQ_UINT(cc, 9);
            LS_EQ_UINT(dt, 0x6);

            /* dmr_burst_colour_code() sits on top and must agree. */
            LS_EQ_UINT(dmr_burst_colour_code(burst), 9);
        }
    }
}

LS_CASE(slot_type_decode_rejects_four_bit_error_that_leaves_no_codeword_within_three)
{
    dmr_lc_t tx = {
        .protect_flag    = 0,
        .flco            = 0,
        .fid             = 0,
        .service_options = 0,
        .destination     = 3100u,
        .source          = 1234567u,
    };

    /* Sweep all C(20,4) = 4845 four-error patterns and count how many the
     * decoder correctly rejects.  With min distance 7, only patterns that
     * happen to land within distance 3 of another codeword can slip past
     * (that requires two codewords at the code's minimum distance, on
     * opposite sides of the received word).  The overwhelming majority
     * must be rejected. */
    int rejected = 0;
    int false_accepts = 0;
    unsigned pattern_detected[4] = { 0, 0, 0, 0 };

    for (unsigned a = 0; a < 20; a++)
    for (unsigned b = a + 1; b < 20; b++)
    for (unsigned c = b + 1; c < 20; c++)
    for (unsigned d = c + 1; d < 20; d++) {
        uint8_t burst[33];
        dmr_gen_burst(burst, k_bs_data, /* CC */ 5,
                      /* DT (rate 3/4 data) */ 0x8, &tx);
        flip_bit(burst, k_slot_type_bits[a]);
        flip_bit(burst, k_slot_type_bits[b]);
        flip_bit(burst, k_slot_type_bits[c]);
        flip_bit(burst, k_slot_type_bits[d]);

        uint8_t packed[3] = { 0, 0, 0 };
        for (unsigned i = 0; i < 10; i++) {
            int bit = (burst[(98u + i) / 8] >> (7u - ((98u + i) & 7u))) & 1;
            if (bit) packed[i / 8] |= (uint8_t)(1u << (7u - (i & 7u)));
        }
        for (unsigned i = 0; i < 10; i++) {
            int bit = (burst[(156u + i) / 8] >> (7u - ((156u + i) & 7u))) & 1;
            unsigned p = 10u + i;
            if (bit) packed[p / 8] |= (uint8_t)(1u << (7u - (p & 7u)));
        }

        uint8_t cc = 0, dt = 0;
        int r = dmr_slot_type_decode(packed, &cc, &dt);
        if (r < 0) {
            rejected++;
            /* Record the first rejected pattern so failure output is useful. */
            if (rejected == 1) {
                pattern_detected[0] = a;
                pattern_detected[1] = b;
                pattern_detected[2] = c;
                pattern_detected[3] = d;
            }
        } else if (cc != 5 || dt != 0x8) {
            /* Decoded to a wrong codeword within distance 3 - a silent
             * mis-decode is exactly the failure mode this test guards
             * against.  Count and keep going so we can report the ratio. */
            false_accepts++;
        }
    }

    LS_CHECK_MSG(rejected > 0,
                 "no 4-bit pattern was detected - the decoder is not "
                 "enforcing its 3-bit correction radius");

    /* Prove the specific detected pattern still trips the reject path when
     * fed on its own, so the assertion is not just a counting artefact. */
    {
        uint8_t burst[33];
        dmr_gen_burst(burst, k_bs_data, 5, 0x8, &tx);
        for (int i = 0; i < 4; i++)
            flip_bit(burst, k_slot_type_bits[pattern_detected[i]]);

        uint8_t packed[3] = { 0, 0, 0 };
        for (unsigned i = 0; i < 10; i++) {
            int bit = (burst[(98u + i) / 8] >> (7u - ((98u + i) & 7u))) & 1;
            if (bit) packed[i / 8] |= (uint8_t)(1u << (7u - (i & 7u)));
        }
        for (unsigned i = 0; i < 10; i++) {
            int bit = (burst[(156u + i) / 8] >> (7u - ((156u + i) & 7u))) & 1;
            unsigned p = 10u + i;
            if (bit) packed[p / 8] |= (uint8_t)(1u << (7u - (p & 7u)));
        }
        uint8_t cc = 0, dt = 0;
        int r = dmr_slot_type_decode(packed, &cc, &dt);
        LS_CHECK_MSG(r < 0,
                     "reproduced 4-bit pattern {%u,%u,%u,%u} decoded as "
                     "cc=%u dt=%u corr=%d, expected rejection",
                     pattern_detected[0], pattern_detected[1],
                     pattern_detected[2], pattern_detected[3],
                     cc, dt, r);
    }

    ls_note("Slot Type Golay(20,8,7): 4-bit patterns rejected=%d "
            "silent-mis-decodes=%d of 4845 total",
            rejected, false_accepts);
}
