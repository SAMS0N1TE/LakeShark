/* LS_TEST_SOURCES: ${DMR_CORE_SRCS}
 *
 * The blocks past the Slot Type: RS(12,9) correction on the full LC, CSBK
 * and data header CRCs and fields, EMB and the embedded LC, talker alias and
 * GPS Info.
 *
 * Where a vector is a constant below, it was built from the ETSI field
 * layouts in a separate Python script - CRCs from Python's own CRC-CCITT
 * (binascii.crc_hqx), bit packing by hand - not by these decoders' encoders,
 * so a decoder that agreed with itself would still have to agree with it.
 * RS correction is exercised against the MMDVM-Host vectors the existing
 * BPTC tests use. No vector here came off the air.
 */

#include "ls_test.h"

#include "dmr.h"
#include "dmr_reference_vectors.h"

#include <string.h>

/* ------------------------------------------------------------ RS(12,9) --- */

LS_CASE(rs129_corrects_any_single_octet_of_the_reference_lcs)
{
    const unsigned n = sizeof(dmr_reference_lc) / sizeof(dmr_reference_lc[0]);
    for (unsigned v = 0; v < n; v++) {
        /* Even entries carry the header mask, odd the terminator's. */
        const uint8_t dt = (v & 1u) ? 2 : 1;
        dmr_lc_t want, got;
        LS_CHECK(dmr_lc_decode(dmr_reference_lc[v].lc, dt, &want));
        for (unsigned pos = 0; pos < 12; pos++)
            for (unsigned e = 1; e < 256; e += 17) {
                uint8_t w[12];
                memcpy(w, dmr_reference_lc[v].lc, 12);
                w[pos] ^= (uint8_t)e;
                /* Checking alone refuses it; correcting recovers it. */
                LS_CHECK(!dmr_lc_decode(w, dt, &got));
                memset(&got, 0, sizeof(got));
                LS_EQ_INT(dmr_lc_correct(w, dt, &got), 2);
                LS_CHECK(memcmp(&got, &want, sizeof(got)) == 0);
            }
        LS_EQ_INT(dmr_lc_correct(dmr_reference_lc[v].lc, dt, &got), 1);
    }
}

LS_CASE(rs129_never_corrects_two_octets_into_a_wrong_lc)
{
    /* Distance four: two octets in error are always at least two from any
       other codeword, so they must be refused, never miscorrected. */
    const uint8_t *ref = dmr_reference_lc[2].lc;
    unsigned tried = 0;
    for (unsigned a = 0; a < 12; a++)
        for (unsigned b = a + 1; b < 12; b++)
            for (unsigned e = 1; e < 256; e += 51) {
                uint8_t w[12];
                memcpy(w, ref, 12);
                w[a] ^= (uint8_t)e;
                w[b] ^= (uint8_t)(e * 7u + 3u);
                dmr_lc_t got;
                LS_EQ_INT(dmr_lc_correct(w, 1, &got), 0);
                tried++;
            }
    LS_CHECK(tried > 300);
}

LS_CASE(rs129_correction_is_masked_by_data_type)
{
    /* A header read as a terminator is two octets of mask off in each parity
       octet - not something correction may paper over. */
    dmr_lc_t got;
    LS_EQ_INT(dmr_lc_correct(dmr_reference_lc[0].lc, 2, &got), 0);
    LS_EQ_INT(dmr_lc_correct(dmr_reference_lc[0].lc, 3, &got), 0);
}

/* ------------------------------------------------------------- CSBK --- */

static const uint8_t CSBK_BS_DWN_ACT[12] =
    {0xB8, 0x00, 0x00, 0x00, 0x01, 0xE2, 0x40, 0x28, 0x00, 0x01, 0x32, 0x4B};
static const uint8_t CSBK_UU_V_REQ[12] =
    {0x84, 0x00, 0x00, 0x00, 0x2F, 0x9B, 0xE5, 0x2F, 0x9B, 0xE6, 0xE2, 0x84};
static const uint8_t CSBK_UU_ANS[12] =
    {0x85, 0x00, 0x00, 0x20, 0x2F, 0x9B, 0xE6, 0x2F, 0x9B, 0xE5, 0x48, 0x88};
static const uint8_t CSBK_PREAMBLE[12] =
    {0xBD, 0x00, 0xC0, 0x05, 0x00, 0x00, 0x09, 0x28, 0x00, 0x01, 0x25, 0x09};

LS_CASE(csbk_bs_outbound_activation_names_the_bs_and_the_mobile)
{
    dmr_csbk_t c;
    LS_CHECK(dmr_csbk_decode(CSBK_BS_DWN_ACT, &c));
    LS_CHECK(c.last_block);
    LS_EQ_UINT(c.opcode, DMR_CSBKO_BS_DWN_ACT);
    LS_EQ_UINT(c.fid, 0);
    LS_CHECK(c.has_addresses);
    LS_EQ_UINT(c.target, 123456);
    LS_EQ_UINT(c.source, 2621441);
    LS_EQ_STR(dmr_csbk_name(c.opcode, c.fid), "BS activation");
}

LS_CASE(csbk_unit_to_unit_request_and_answer)
{
    dmr_csbk_t c;
    LS_CHECK(dmr_csbk_decode(CSBK_UU_V_REQ, &c));
    LS_EQ_UINT(c.opcode, DMR_CSBKO_UU_V_REQ);
    LS_EQ_UINT(c.target, 3120101);
    LS_EQ_UINT(c.source, 3120102);
    LS_EQ_UINT(c.service_options, 0);

    LS_CHECK(dmr_csbk_decode(CSBK_UU_ANS, &c));
    LS_EQ_UINT(c.opcode, DMR_CSBKO_UU_ANS_RSP);
    LS_EQ_UINT(c.answer, 0x20);           /* proceed */
    LS_EQ_UINT(c.target, 3120102);
    LS_EQ_UINT(c.source, 3120101);
}

LS_CASE(csbk_preamble_announces_group_data_and_its_length)
{
    dmr_csbk_t c;
    LS_CHECK(dmr_csbk_decode(CSBK_PREAMBLE, &c));
    LS_EQ_UINT(c.opcode, DMR_CSBKO_PREAMBLE);
    LS_CHECK(c.preamble_data);
    LS_CHECK(c.preamble_group);
    LS_EQ_UINT(c.blocks_to_follow, 5);
    LS_EQ_UINT(c.target, 9);
    LS_EQ_UINT(c.source, 2621441);
}

LS_CASE(csbk_crc_refuses_every_single_bit_and_the_wrong_mask)
{
    for (unsigned bit = 0; bit < 96; bit++) {
        uint8_t w[12];
        memcpy(w, CSBK_PREAMBLE, 12);
        w[bit >> 3] ^= (uint8_t)(0x80u >> (bit & 7u));
        dmr_csbk_t c;
        memset(&c, 0x5A, sizeof(c));
        LS_CHECK(!dmr_csbk_decode(w, &c));
        LS_EQ_UINT(c.opcode, 0x5A);       /* untouched on refusal */
    }
    /* The same block under the data header mask is not a CSBK. */
    dmr_data_header_t h;
    LS_CHECK(!dmr_data_header_decode(CSBK_PREAMBLE, &h));
    LS_CHECK(dmr_block_crc_ok(CSBK_PREAMBLE, DMR_CRC_MASK_CSBK));
}

/* ------------------------------------------------------- data header --- */

static const uint8_t DH_UNCONFIRMED[12] =
    {0x82, 0x43, 0x00, 0x00, 0x09, 0x28, 0x00, 0x01, 0x83, 0x08, 0xC1, 0x98};
static const uint8_t DH_RESPONSE[12] =
    {0x01, 0x40, 0x28, 0x00, 0x01, 0x2F, 0x9B, 0xE5, 0x00, 0x08, 0x5C, 0xB9};

LS_CASE(data_header_unconfirmed_group_packet)
{
    dmr_data_header_t h;
    LS_CHECK(dmr_data_header_decode(DH_UNCONFIRMED, &h));
    LS_EQ_UINT(h.dpf, DMR_DPF_UNCONFIRMED);
    LS_CHECK(h.group);
    LS_CHECK(!h.response_requested);
    LS_EQ_UINT(h.sap, 4);
    LS_EQ_UINT(h.pad_octets, 3);
    LS_EQ_UINT(h.destination, 9);
    LS_EQ_UINT(h.source, 2621441);
    LS_CHECK(h.full_message);
    LS_EQ_UINT(h.blocks_to_follow, 3);
    LS_EQ_UINT(h.fsn, 8);
    LS_EQ_STR(dmr_dpf_name(h.dpf), "unconfirmed");
}

LS_CASE(data_header_response_carries_class_type_status)
{
    dmr_data_header_t h;
    LS_CHECK(dmr_data_header_decode(DH_RESPONSE, &h));
    LS_EQ_UINT(h.dpf, DMR_DPF_RESPONSE);
    LS_CHECK(!h.group);
    LS_EQ_UINT(h.destination, 2621441);
    LS_EQ_UINT(h.source, 3120101);
    LS_EQ_UINT(h.rsp_class, 0);
    LS_EQ_UINT(h.rsp_type, 1);            /* ACK */
    LS_EQ_UINT(h.rsp_status, 0);
    dmr_csbk_t c;
    LS_CHECK(!dmr_csbk_decode(DH_RESPONSE, &c));
}

/* -------------------------------------------------------------- EMB --- */

LS_CASE(emb_is_the_qr_16_7_6_code_with_distance_six)
{
    /* Built independently from the generator x^8+x^5+x^4+x^3+1 plus even
       parity; the first five agree with the published QR(16,7,6) table. */
    LS_EQ_UINT(dmr_emb_encode(0, false, 1), 0x0273);
    LS_EQ_UINT(dmr_emb_encode(0, false, 2), 0x04E5);
    LS_EQ_UINT(dmr_emb_encode(0, true,  0), 0x09C9);
    LS_EQ_UINT(dmr_emb_encode(1, false, 0), 0x11E2);
    LS_EQ_UINT(dmr_emb_encode(2, false, 0), 0x21B7);
    LS_EQ_UINT(dmr_emb_encode(7, false, 1), 0x7338);
    LS_EQ_UINT(dmr_emb_encode(15, true, 3), 0xFE5B);

    int dmin = 99;
    for (unsigned a = 0; a < 128; a++)
        for (unsigned b = a + 1; b < 128; b++) {
            uint32_t x = (uint32_t)(dmr_emb_encode((uint8_t)(a >> 3), (a >> 2) & 1u, (uint8_t)(a & 3u)) ^
                                    dmr_emb_encode((uint8_t)(b >> 3), (b >> 2) & 1u, (uint8_t)(b & 3u)));
            int w = 0;
            for (; x; x &= x - 1u) w++;
            if (w < dmin) dmin = w;
        }
    LS_EQ_INT(dmin, 6);
}

LS_CASE(emb_decode_corrects_two_and_refuses_three)
{
    const uint16_t cw = dmr_emb_encode(11, true, DMR_LCSS_CONT);
    uint8_t cc, lcss;
    bool pi;
    LS_EQ_INT(dmr_emb_decode(cw, &cc, &pi, &lcss), 0);
    LS_EQ_UINT(cc, 11);
    LS_CHECK(pi);
    LS_EQ_UINT(lcss, DMR_LCSS_CONT);
    for (unsigned i = 0; i < 16; i++)
        for (unsigned j = i + 1; j < 16; j++) {
            const uint16_t w = (uint16_t)(cw ^ (1u << i) ^ (1u << j));
            LS_EQ_INT(dmr_emb_decode(w, &cc, &pi, &lcss), 2);
            LS_EQ_UINT(cc, 11);
        }
    /* Three errors: either refused or, if nearer another word, at least not
       claimed as a two-error fix of this one. */
    const uint16_t w3 = (uint16_t)(cw ^ 0x0007u);
    const int e = dmr_emb_decode(w3, &cc, &pi, &lcss);
    LS_CHECK(e < 0 || cc != 11 || lcss != DMR_LCSS_CONT || !pi);
}

/* ------------------------------------------------------- embedded LC --- */

static const uint8_t EMB_LC9[9] = {0x00, 0x00, 0x00, 0x00, 0x0C, 0x64, 0x28, 0x00, 0x01};
static const uint32_t EMB_FRAGS[4] = {0x0C000009u, 0x11110000u, 0x09123A1Du, 0x0321273Cu};

LS_CASE(embedded_lc_matches_the_independent_layout)
{
    uint32_t frag[4];
    dmr_emb_lc_encode(EMB_LC9, frag);
    for (unsigned i = 0; i < 4; i++) LS_EQ_UINT(frag[i], EMB_FRAGS[i]);
    uint8_t lc9[9];
    LS_EQ_INT(dmr_emb_lc_decode(EMB_FRAGS, lc9), 0);
    LS_CHECK(memcmp(lc9, EMB_LC9, 9) == 0);
}

LS_CASE(embedded_lc_corrects_one_bit_per_row_and_refuses_a_bad_checksum)
{
    uint8_t lc9[9];
    /* One bit in each of the seven Hamming rows: wire bit k is row k % 8. */
    uint32_t f[4];
    memcpy(f, EMB_FRAGS, sizeof(f));
    for (unsigned row = 0; row < 7; row++) {
        const unsigned k = row + 8u * (row * 2u + 1u);       /* column 2r+1 */
        f[k / 32] ^= 1u << (31 - k % 32);
    }
    LS_EQ_INT(dmr_emb_lc_decode(f, lc9), 7);
    LS_CHECK(memcmp(lc9, EMB_LC9, 9) == 0);

    /* Two in one row is detected, not miscorrected. */
    memcpy(f, EMB_FRAGS, sizeof(f));
    f[0] ^= (1u << 31) | (1u << 23);                          /* row 0, cols 0, 1 */
    LS_EQ_INT(dmr_emb_lc_decode(f, lc9), -1);

    /* The same LC with its checksum off by one bit, every row and column
       consistent with that (built in the script): only the checksum can
       refuse it. */
    const uint32_t bad_cs[4] = {0x0C000009u, 0x11110000u, 0x0912391Du, 0x0322243Fu};
    LS_EQ_INT(dmr_emb_lc_decode(bad_cs, lc9), -1);

    /* Fragments from two different LCs: rows can pass, the column parity or
       checksum must not. */
    uint8_t other[9];
    memcpy(other, EMB_LC9, 9);
    other[8] ^= 0x01;
    uint32_t g[4];
    dmr_emb_lc_encode(other, g);
    LS_EQ_INT(dmr_emb_lc_decode(g, lc9), 0);
    LS_CHECK(memcmp(lc9, other, 9) == 0);
    uint32_t mix[4] = { EMB_FRAGS[0], EMB_FRAGS[1], EMB_FRAGS[2], g[3] };
    LS_CHECK(g[3] != EMB_FRAGS[3]);
    LS_EQ_INT(dmr_emb_lc_decode(mix, lc9), -1);
}

LS_CASE(embedded_lc_reassembles_in_lcss_order_only)
{
    dmr_emb_asm_t a;
    uint8_t lc9[9];
    dmr_emb_asm_reset(&a);
    LS_EQ_INT(dmr_emb_asm_feed(&a, DMR_LCSS_FIRST, EMB_FRAGS[0], lc9), 0);
    LS_EQ_INT(dmr_emb_asm_feed(&a, DMR_LCSS_CONT,  EMB_FRAGS[1], lc9), 0);
    LS_EQ_INT(dmr_emb_asm_feed(&a, DMR_LCSS_CONT,  EMB_FRAGS[2], lc9), 0);
    LS_EQ_INT(dmr_emb_asm_feed(&a, DMR_LCSS_LAST,  EMB_FRAGS[3], lc9), 1);
    LS_CHECK(memcmp(lc9, EMB_LC9, 9) == 0);

    /* A lost burst C: the last fragment arrives after two, not three. */
    dmr_emb_asm_reset(&a);
    dmr_emb_asm_feed(&a, DMR_LCSS_FIRST, EMB_FRAGS[0], lc9);
    dmr_emb_asm_feed(&a, DMR_LCSS_CONT,  EMB_FRAGS[2], lc9);
    LS_EQ_INT(dmr_emb_asm_feed(&a, DMR_LCSS_LAST, EMB_FRAGS[3], lc9), 0);
    /* Continuation with no first. */
    dmr_emb_asm_reset(&a);
    LS_EQ_INT(dmr_emb_asm_feed(&a, DMR_LCSS_CONT, EMB_FRAGS[1], lc9), 0);
    LS_EQ_UINT(a.have, 0);
}

LS_CASE(an_embedded_fragment_and_emb_come_out_of_the_burst_middle)
{
    uint8_t burst[33];
    memset(burst, 0, sizeof(burst));
    const uint16_t emb = dmr_emb_encode(7, false, DMR_LCSS_FIRST);
    const uint32_t frag = 0xDEADBEEFu;
    for (unsigned i = 0; i < 8; i++) {
        if ((emb >> (15 - i)) & 1u) burst[(108 + i) >> 3] |= (uint8_t)(0x80u >> ((108 + i) & 7u));
        if ((emb >> (7 - i)) & 1u)  burst[(148 + i) >> 3] |= (uint8_t)(0x80u >> ((148 + i) & 7u));
    }
    for (unsigned i = 0; i < 32; i++)
        if ((frag >> (31 - i)) & 1u) burst[(116 + i) >> 3] |= (uint8_t)(0x80u >> ((116 + i) & 7u));
    LS_EQ_UINT(dmr_burst_emb(burst), emb);
    LS_EQ_UINT(dmr_burst_embedded(burst), frag);
}

/* ------------------------------------------------------- talker alias --- */

static const uint8_t TA7[2][9] = {
    {0x04, 0x00, 0x15, 0x39, 0x84, 0x38, 0x33, 0x26, 0x20},
    {0x05, 0x00, 0x85, 0xBF, 0x10, 0x00, 0x00, 0x00, 0x00},
};
static const uint8_t TA8[2][9] = {
    {0x04, 0x00, 0x56, 0x4A, 0xF6, 0x72, 0x67, 0x2D, 0x44},
    {0x05, 0x00, 0x4C, 0x31, 0x41, 0x42, 0x43, 0x00, 0x00},
};
static const uint8_t TA16[3][9] = {
    {0x04, 0x00, 0xCE, 0x03, 0xA9, 0x00, 0x6D, 0x00, 0x65},
    {0x05, 0x00, 0x00, 0x67, 0x00, 0x61, 0x00, 0x20, 0x4E},
    {0x06, 0x00, 0x2D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

LS_CASE(talker_alias_seven_bit_builds_as_blocks_arrive)
{
    dmr_alias_t a;
    dmr_alias_reset(&a);
    LS_CHECK(dmr_alias_feed(&a, TA7[0]));
    LS_EQ_STR(a.text, "N0CALL ");          /* 49 bits: seven characters */
    LS_CHECK(!a.complete);
    LS_CHECK(dmr_alias_feed(&a, TA7[1]));
    LS_EQ_STR(a.text, "N0CALL Bob");
    LS_CHECK(a.complete);
    LS_EQ_UINT(a.format, 0);
    LS_EQ_UINT(a.length, 10);
}

LS_CASE(talker_alias_iso_8859_1_becomes_utf8)
{
    dmr_alias_t a;
    dmr_alias_reset(&a);
    /* A block before its header is held, but says nothing on its own. */
    LS_CHECK(dmr_alias_feed(&a, TA8[1]));
    LS_EQ_STR(a.text, "");
    LS_CHECK(dmr_alias_feed(&a, TA8[0]));
    LS_EQ_STR(a.text, "J\xC3\xB6rg-DL1ABC");
    LS_CHECK(a.complete);
}

LS_CASE(talker_alias_utf16_including_a_character_split_across_blocks)
{
    dmr_alias_t a;
    dmr_alias_reset(&a);
    for (unsigned i = 0; i < 3; i++) LS_CHECK(dmr_alias_feed(&a, TA16[i]));
    LS_EQ_STR(a.text, "\xCE\xA9mega \xE4\xB8\xAD");
    LS_CHECK(a.complete);
}

LS_CASE(talker_alias_ignores_other_lcs_and_restarts_on_a_new_header)
{
    dmr_alias_t a;
    dmr_alias_reset(&a);
    const uint8_t voice[9] = {0x00, 0x00, 0x00, 0x00, 0x0C, 0x64, 0x28, 0x00, 0x01};
    LS_CHECK(!dmr_alias_feed(&a, voice));
    uint8_t mfr[9];
    memcpy(mfr, TA7[0], 9);
    mfr[1] = 0x10;                         /* manufacturer FID: not ETSI TA */
    LS_CHECK(!dmr_alias_feed(&a, mfr));
    dmr_alias_feed(&a, TA7[0]);
    dmr_alias_feed(&a, TA7[1]);
    dmr_alias_feed(&a, TA8[0]);            /* different format and length */
    LS_EQ_STR(a.text, "J\xC3\xB6rg-D");
    LS_CHECK(!a.complete);
}

/* -------------------------------------------------------------- GPS --- */

LS_CASE(gps_info_decodes_both_hemispheres)
{
    const uint8_t berlin[9] = {0x08, 0x00, 0x02, 0x13, 0x10, 0x9D, 0x4A, 0xB1, 0xF3};
    const uint8_t baires[9] = {0x08, 0x00, 0x05, 0xAC, 0xF7, 0xE8, 0xCE, 0xC9, 0x2D};
    dmr_gps_t g;
    LS_CHECK(dmr_gps_decode(berlin, &g));
    LS_EQ_UINT(g.position_error, 1);
    LS_EQ_INT(g.latitude_raw, 4895219);
    LS_EQ_INT(g.longitude_raw, 1249437);
    LS_NEAR(g.latitude, 52.5200, 0.0001);
    LS_NEAR(g.longitude, 13.4050, 0.0001);

    LS_CHECK(dmr_gps_decode(baires, &g));
    LS_EQ_UINT(g.position_error, 2);
    LS_NEAR(g.latitude, -34.6037, 0.0001);
    LS_NEAR(g.longitude, -58.3816, 0.0001);

    const uint8_t voice[9] = {0x00, 0x00, 0x00, 0x00, 0x0C, 0x64, 0x28, 0x00, 0x01};
    LS_CHECK(!dmr_gps_decode(voice, &g));
}
