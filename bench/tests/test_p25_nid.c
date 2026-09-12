/* LS_TEST_SOURCES: */
/* P25 NID: BCH(63,16) over NAC + DUID.

   Including the .c gives the test the file's static bch_encode(), so a
   codeword can be built the same way the decoder expects to read one. */

#include "ls_test.h"
#include "p25p1_check_nid.c"

static void make_codeword(int nac12, int duid_hi, int duid_lo, char *cw63)
{
    int data[BCH_KK];
    for (int i = 0; i < 12; i++) data[i] = (nac12 >> (11 - i)) & 1;
    data[12] = (duid_hi >> 1) & 1;
    data[13] = (duid_hi >> 0) & 1;
    data[14] = (duid_lo >> 1) & 1;
    data[15] = (duid_lo >> 0) & 1;

    int par[BCH_RR];
    bch_encode(data, par);

    for (int i = 0; i < BCH_KK; i++) cw63[i] = (char)data[i];
    for (int i = 0; i < BCH_RR; i++) cw63[BCH_KK + i] = (char)par[i];
}

LS_CASE(clean_nid_round_trips)
{
    const int nacs[]  = { 0x000, 0x001, 0x293, 0x1F0, 0xFFE, 0xFFF };
    const int duids[] = { 0x0, 0x3, 0x5, 0x7, 0xA, 0xC, 0xF };

    for (unsigned n = 0; n < sizeof(nacs) / sizeof(nacs[0]); n++) {
        for (unsigned d = 0; d < sizeof(duids) / sizeof(duids[0]); d++) {
            char cw[63];
            make_codeword(nacs[n], (duids[d] >> 2) & 3, duids[d] & 3, cw);

            int  got_nac = -1, ec = -1;
            char got_duid[4] = { 0 };
            int  ok = check_NID_ec(cw, &got_nac, got_duid, 0, &ec);

            LS_CHECK_MSG(ok, "NAC %03X DUID %X rejected clean", nacs[n], duids[d]);
            LS_EQ_INT(got_nac, nacs[n]);
            LS_EQ_INT(ec, 0);

            char want[3];
            want[0] = (char)('0' + ((duids[d] >> 2) & 3));
            want[1] = (char)('0' + (duids[d] & 3));
            want[2] = 0;
            LS_EQ_STR(got_duid, want);
        }
    }
}

LS_CASE(corrects_up_to_eleven_errors)
{
    /* BCH(63,16) is t=11. Anything less than full correction at 11 means the
       Berlekamp-Massey loop has regressed. */
    ls_rng_t rng;
    ls_rng_seed(&rng, 0x5EED);

    for (int n_err = 0; n_err <= 11; n_err++) {
        int recovered = 0;
        const int trials = 40;

        for (int t = 0; t < trials; t++) {
            int nac = (int)(ls_rng_u32(&rng) & 0xFFF);
            int dh  = (int)(ls_rng_u32(&rng) & 3);
            int dl  = (int)(ls_rng_u32(&rng) & 3);

            char cw[63];
            make_codeword(nac, dh, dl, cw);

            char rx[63];
            memcpy(rx, cw, sizeof(rx));

            int used[63] = { 0 };
            for (int e = 0; e < n_err; e++) {
                int p;
                do { p = (int)(ls_rng_u32(&rng) % 63u); } while (used[p]);
                used[p] = 1;
                rx[p] = (char)(rx[p] ? 0 : 1);
            }

            int  got_nac = -1, ec = -1;
            char got_duid[4] = { 0 };
            if (check_NID_ec(rx, &got_nac, got_duid, 0, &ec)
                && got_nac == nac
                && got_duid[0] == (char)('0' + dh)
                && got_duid[1] == (char)('0' + dl)) {
                recovered++;
                LS_CHECK_MSG(ec == n_err, "%d errors reported as %d", n_err, ec);
            }
        }

        LS_CHECK_MSG(recovered == trials,
                     "%d-error correction: %d/%d recovered", n_err, recovered, trials);
    }
}

LS_CASE(rejects_uncorrectable_garbage)
{
    /* Random 63-bit words must not be waved through as valid NIDs. A false
       accept here is a phantom NAC on the display. */
    ls_rng_t rng;
    ls_rng_seed(&rng, 0xF00D);

    int accepted = 0;
    const int trials = 2000;

    for (int t = 0; t < trials; t++) {
        char rx[63];
        for (int i = 0; i < 63; i++) rx[i] = (char)(ls_rng_u32(&rng) & 1);

        int  nac = -1, ec = -1;
        char duid[4] = { 0 };
        if (check_NID_ec(rx, &nac, duid, 0, &ec)) accepted++;
    }

    /* 2^16 codewords in 2^63 words, but the decoder corrects out to t=11, so
       a small false-accept rate is inherent. It must stay small. */
    ls_note("false accepts: %d / %d", accepted, trials);
    LS_CHECK_MSG(accepted * 100 <= trials * 5,
                 "false accept rate %d/%d is too high", accepted, trials);
}

LS_CASE(every_p25_duid_decodes_to_its_dsd_name)
{
    /* dsd_frame.c dispatches on the two-dibit spelling of the DUID, so this
       is the table that connects the wire encoding to the branch that runs.
       "13" is the TSDU: recognised and labelled today, then skipped with
       skipDibit(328 - 25) - the payload is thrown away, which is why there
       is no trunking. When TSBK parsing lands it replaces that one skip, and
       this case is what guarantees it will be reached. */
    const struct { int duid; const char *dibits; const char *name; } tbl[] = {
        { 0x0, "00", "HDU"   },
        { 0x3, "03", "TDU"   },
        { 0x5, "11", "LDU1"  },
        { 0x7, "13", "TSDU"  },
        { 0xA, "22", "LDU2"  },
        { 0xC, "30", "PDU"   },
        { 0xF, "33", "TDULC" },
    };

    for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        char cw[63];
        make_codeword(0x293, (tbl[i].duid >> 2) & 3, tbl[i].duid & 3, cw);

        int  nac = -1, ec = -1;
        char duid[4] = { 0 };
        LS_CHECK_MSG(check_NID_ec(cw, &nac, duid, 0, &ec),
                     "%s (DUID %X) rejected", tbl[i].name, tbl[i].duid);
        LS_EQ_INT(nac, 0x293);
        LS_CHECK_MSG(strcmp(duid, tbl[i].dibits) == 0,
                     "%s: DUID %X spelled [%s], dsd_frame.c matches [%s]",
                     tbl[i].name, tbl[i].duid, duid, tbl[i].dibits);
    }
}
