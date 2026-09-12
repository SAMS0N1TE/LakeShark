/* LS_TEST_SOURCES: ${APP}/p25/p25_ess.c */
/* P25 Encryption Sync Stream (ESS) mute gate. */

#include "ls_test.h"
#include "dsd.h"

#include <string.h>

LS_CASE(clear_call_algid_0x80_is_not_muted)
{
    /* The straightforward case: a valid ESS says CLEAR, and audio flows. */
    dsd_state state;
    dsd_opts opts;
    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));

    state.p25_algid = 0x80;
    state.p25_kid = 0;
    state.p25_ess_valid = 1;
    opts.unmute_encrypted_p25 = 0;

    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 0);
}

LS_CASE(adp_call_algid_0x84_kid_0_is_muted)
{
    /* The regression this whole task exists to fix. ADP calls legitimately
     * carry KID 0, so a KID-based test lets them through the vocoder. */
    dsd_state state;
    dsd_opts opts;
    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));

    state.p25_algid = 0x84;
    state.p25_kid = 0;
    state.p25_ess_valid = 1;
    opts.unmute_encrypted_p25 = 0;

    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 1);
    /* And the algorithm name is what the UI will render on that muted call. */
    LS_EQ_STR(p25_algid_name(0x84), "ADP");
}

LS_CASE(aes_call_algid_0x89_is_muted)
{
    dsd_state state;
    dsd_opts opts;
    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));

    state.p25_algid = 0x89;
    state.p25_ess_valid = 1;
    opts.unmute_encrypted_p25 = 0;

    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 1);
    LS_EQ_STR(p25_algid_name(0x89), "AES-256");
}

LS_CASE(clear_call_after_encrypted_call_is_not_muted)
{
    /* The regression that closing tasks 1-3 would otherwise create: if the
     * ESS from an encrypted call persists past its terminator, the next
     * clear call inherits the mute. TDU / TDULC / talkgroup change all
     * route through p25_ess_clear; drive that here. */
    dsd_state state;
    dsd_opts opts;
    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));

    state.p25_algid = 0x84;
    state.p25_ess_valid = 1;
    opts.unmute_encrypted_p25 = 0;
    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 1);

    p25_ess_clear(&state);
    LS_EQ_INT(state.p25_ess_valid, 0);
    LS_EQ_INT(state.p25_algid, 0);
    LS_EQ_INT(state.p25_kid, 0);

    /* Now a fresh clear call. ESS has to land again before we can mute. */
    state.p25_algid = 0x80;
    state.p25_ess_valid = 1;
    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 0);
}

LS_CASE(ldu1_before_any_ldu2_mutes_until_algid_is_known)
{

    dsd_state state;
    dsd_opts opts;
    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));

    state.p25_ess_valid = 0;
    state.p25_algid = 0;    /* zero is not a valid ALGID - explicit for clarity */
    opts.unmute_encrypted_p25 = 0;

    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 1);

    opts.unmute_encrypted_p25 = 1;
    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 0);
}

LS_CASE(algid_is_encrypted_predicate_matches_clear_only)
{
    /* the grant follower consults this to decide whether to hand
     * control back after the first LDU2 lands. It must classify 0x80 as
     * clear and every other listed algorithm as encrypted. Vendor / unknown
     * algids (e.g. 0x00 pre-ESS) are treated as encrypted so a followed call
     * cannot leak audio while the identity of the algorithm is uncertain. */
    LS_EQ_INT(p25_algid_is_encrypted(0x80), 0);
    LS_EQ_INT(p25_algid_is_encrypted(0x84), 1);
    LS_EQ_INT(p25_algid_is_encrypted(0x89), 1);
    LS_EQ_INT(p25_algid_is_encrypted(0x9F), 1);
    LS_EQ_INT(p25_algid_is_encrypted(0x00), 1);
    LS_EQ_INT(p25_algid_is_encrypted(0xFF), 1);
}

/*
 * Hamming(10,6,3) - transcribed from components/lakeshark/apps/p25/Hamming.hpp
 * so this test can run in a C-only bench. The firmware's implementation is a
 * lookup-table wrapper around the same matrices; if that ever moves, this
 * test needs the same constants updated (they are pinned by P25 CAI, so a
 * drift is exceedingly unlikely).
 *
 * Layout: hex[0..5] occupy bits 9..4 of the codeword; parity[0..3] occupy
 * bits 3..0. h0..h3 are the parity-check rows.
 */
static int popcount_and_1(unsigned v) { int n = 0; while (v) { n ^= 1; v &= v - 1; } return n; }

static int hamming_10_6_3_syndrome(unsigned codeword)
{
    static const unsigned h[4] = {
        /* "1110011000" */ 0x398u,
        /* "1101010100" */ 0x354u,
        /* "1011100010" */ 0x2E2u,
        /* "0111100001" */ 0x1E1u,
    };
    return (popcount_and_1(codeword & h[0]) << 3) |
           (popcount_and_1(codeword & h[1]) << 2) |
           (popcount_and_1(codeword & h[2]) << 1) |
            popcount_and_1(codeword & h[3]);
}

static int hamming_10_6_3_bad_bit(int syndrome)
{
    /* bad_bit_table from Hamming.hpp. -1 = 2 errors, -2 = impossible. */
    switch (syndrome) {
        case  1: return 0;  case  2: return 1;  case  4: return 2;
        case  8: return 3;  case 12: return 4;  case  3: return 5;
        case  7: return 6;  case 11: return 7;  case 13: return 8;
        case 14: return 9;
        default: return -1;
    }
}

static unsigned hamming_10_6_3_encode(unsigned data6)
{
    /* g0..g5 from Hamming.hpp. Each is the row for one data bit. */
    static const unsigned g[6] = {
        /* "1000001110" */ 0x20Eu,
        /* "0100001101" */ 0x10Du,
        /* "0010001011" */ 0x08Bu,
        /* "0001000111" */ 0x047u,
        /* "0000100011" */ 0x023u,
        /* "0000011100" */ 0x01Cu,
    };
    unsigned cw = 0;
    for (int i = 0; i < 6; i++)
        if ((data6 >> (5 - i)) & 1) cw ^= g[i];
    return cw;
}

static unsigned hamming_10_6_3_correct(unsigned cw, int *err_count)
{
    int syn = hamming_10_6_3_syndrome(cw);
    if (syn == 0) { *err_count = 0; return cw; }
    int bit = hamming_10_6_3_bad_bit(syn);
    if (bit < 0) { *err_count = 2; return cw; }
    *err_count = 1;
    if (bit >= 4)                /* data-bit error - flip it */
        cw ^= (1u << bit);
    return cw;
}

LS_CASE(algid_survives_one_bit_error_in_hamming_10_6_3)
{
    /* ALGID is 8 bits split across two hex_data words in the ESS: the top
     * six bits sit in hex_data[3] and the bottom two in hex_data[2][0..1].
     * If a single bit flips in the top hex word, Hamming(10,6,3) must fix
     * it before the ESS extraction reads algid[0..5]. Test that the same
     * matrices the firmware uses recover every single-bit error position. */
    const unsigned adp_top6 = 0x21;  /* 0x84 = 10000100b, top 6 = 100001b */
    unsigned cw = hamming_10_6_3_encode(adp_top6);

    /* Round-trip: no error. */
    int ec = -1;
    unsigned rx = hamming_10_6_3_correct(cw, &ec);
    LS_EQ_INT(ec, 0);
    LS_EQ_UINT((rx >> 4) & 0x3F, adp_top6);

    /* Every single-bit error is a t=1 case for Hamming(10,6,3) and must be
     * recovered when it lands on a data bit; parity-bit errors are counted
     * but do not corrupt the message, so the data half stays intact either
     * way. */
    for (int b = 0; b < 10; b++) {
        unsigned bad = cw ^ (1u << b);
        int ec2 = -1;
        unsigned fixed = hamming_10_6_3_correct(bad, &ec2);
        LS_CHECK_MSG(ec2 == 1, "bit %d not counted as a single error", b);
        LS_CHECK_MSG(((fixed >> 4) & 0x3F) == adp_top6,
                     "bit %d corrupted ALGID: got 0x%02X, want 0x%02X",
                     b, (unsigned)((fixed >> 4) & 0x3F), adp_top6);
    }
}
