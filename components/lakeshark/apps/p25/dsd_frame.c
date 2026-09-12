/*
 * DSD-derived source. Attribution restored in LakeShark on 2026-09-11 from
 * the DSD COPYRIGHT at revision
 * 59423fa46be8b41ef0bd2f3d2b45590600be29f0:
 * https://github.com/szechyjs/dsd/blob/59423fa46be8b41ef0bd2f3d2b45590600be29f0/src/dsd_frame.c
 *
 * That revision is a verified comparison source, established by comparing
 * identifiers and literals after comments and whitespace were removed. It is
 * not a claim about which revision or intervening fork was originally
 * imported. This file has been modified for LakeShark and the ESP32-P4.
 *
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "dsd.h"
#include "diag.h"
#if !defined(NULL)
#define NULL 0
#endif

#ifndef AUDIO_HAL_H
typedef int beep_kind_t;
extern void audio_beep_request(beep_kind_t kind);
#define BEEP_SYNC_SUCCESS ((beep_kind_t)1)
#endif

#include "p25p1_check_nid.h"
#include "p25_tsbk.h"

extern int autoscan_bch_ok_flag;
extern int dsd_bch_fail_counter;

static void
captureTSDU(dsd_opts *opts, dsd_state *state)
{
    if (state->p25_frame_valid) {
        /* two systems may reuse IDEN and TG values. A validated new
         * control NAC retires the old plan before even the first grant. */
        if (state->p25_control_nac_valid && state->p25_control_nac != state->nac)
            p25_tsbk_reset_system(state);
        state->p25_control_nac = (uint16_t)state->nac;
        state->p25_control_nac_valid = 1;
    }
    state->p25_tsdu_dibit_count = 0;
    for (unsigned int i = 0; i < P25_TSDU_DIBIT_COUNT; i++)
        state->p25_tsdu_dibits[i] = (uint8_t)getDibit(opts, state);
    state->p25_tsdu_dibit_count = P25_TSDU_DIBIT_COUNT;
    state->p25_frame_tsbks = (uint8_t)p25_tsbk_process_tsdu(
        state, state->p25_tsdu_dibits, state->p25_tsdu_dibit_count);
}

void
printFrameInfo(dsd_opts *opts, dsd_state *state)
{
    int level = (int)state->max / 164;
    if (opts->verbose > 0)
        printf("inlvl: %2i%% ", level);
    if (state->nac != 0)
        printf("nac: %4X ", state->nac);
    if (opts->verbose > 1)
        printf("src: %8i ", state->lastsrc);
    printf("tg: %5i ", state->lasttg);
}

void
processFrame(dsd_opts *opts, dsd_state *state)
{
    int i, j, dibit;
    char duid[3];
    char nac[13];
    char status_0;
    char bch_code[63];
    int index_bch_code;
    unsigned char parity;
    char v;
    int new_nac;
    char new_duid[3];
    int check_result;
    int bch_ec = -1;

    int nid_dibits[33] = {0};

    state->p25_frame_valid = 0;
    state->p25_frame_tsbks = 0;

    nac[12] = 0;
    duid[2] = 0;
    j = 0;

    state->center = ((state->max) + (state->min)) / 2;
    state->umid = (((state->max) - state->center) / 2) + state->center;
    state->lmid = (((state->min) - state->center) / 2) + state->center;

    if (state->rf_mod == 1) {
        state->maxref = (int)(state->max * 0.80F);
        state->minref = (int)(state->min * 0.80F);
    } else {
        state->maxref = state->max;
        state->minref = state->min;
    }

    j = 0;
    index_bch_code = 0;

    for (i = 0; i < 6; i++) {
        dibit = getDibit(opts, state);
        nid_dibits[i] = dibit;
        v = 1 & (dibit >> 1);
        nac[j++] = v + '0';
        bch_code[index_bch_code++] = v;
        v = 1 & dibit;
        nac[j++] = v + '0';
        bch_code[index_bch_code++] = v;
    }
    state->nac = strtol(nac, NULL, 2);

    for (i = 0; i < 2; i++) {
        dibit = getDibit(opts, state);
        nid_dibits[6 + i] = dibit;
        duid[i] = dibit + '0';
        bch_code[index_bch_code++] = 1 & (dibit >> 1);
        bch_code[index_bch_code++] = 1 & dibit;
    }

    for (i = 0; i < 3; i++) {
        dibit = getDibit(opts, state);
        nid_dibits[8 + i] = dibit;
        bch_code[index_bch_code++] = 1 & (dibit >> 1);
        bch_code[index_bch_code++] = 1 & dibit;
    }

    status_0 = getDibit(opts, state) + '0';
    (void)status_0;

    for (i = 0; i < 20; i++) {
        dibit = getDibit(opts, state);
        nid_dibits[11 + i] = dibit;
        bch_code[index_bch_code++] = 1 & (dibit >> 1);
        bch_code[index_bch_code++] = 1 & dibit;
    }

    dibit = getDibit(opts, state);
    nid_dibits[31] = dibit;
    bch_code[index_bch_code] = 1 & (dibit >> 1);
    parity = (1 & dibit);
    nid_dibits[32] = parity;

    check_result = check_NID_ec(bch_code, &new_nac, new_duid, parity, &bch_ec);

    diag_count_bch_result(check_result, bch_ec);

    if (check_result) {
        autoscan_bch_ok_flag++;

        audio_beep_request(BEEP_SYNC_SUCCESS);

        static int ok_ctr = 0;
        if ((++ok_ctr % 10) == 0) {
            diag_dump_nid("OKDMP", nid_dibits, new_nac, new_duid, bch_ec, 1, NULL);
        }
    } else {
        dsd_bch_fail_counter++;

        static int fail_ctr = 0;
        if ((++fail_ctr & 3) == 0) {
            diag_dump_nid("FLDMP", nid_dibits, state->nac, duid, bch_ec, 0,
                          "uncorrectable");
        }
    }

    if (check_result) {
        if (new_nac != state->nac) {
            state->nac = new_nac;
            state->debug_header_errors++;
        }
        if (strcmp(new_duid, duid) != 0) {
            duid[0] = new_duid[0];
            duid[1] = new_duid[1];
            state->debug_header_errors++;
        }
    } else {

        if (strcmp(duid, "00") == 0 || strcmp(duid, "03") == 0 ||
            strcmp(duid, "11") == 0 || strcmp(duid, "13") == 0 ||
            strcmp(duid, "22") == 0 || strcmp(duid, "30") == 0 ||
            strcmp(duid, "33") == 0) {
            state->debug_header_critical_errors++;
        } else {
            duid[0] = 'E';
            duid[1] = 'E';
            state->debug_header_critical_errors++;
        }
    }

    if (!check_result) {
        state->pcm_out_write = 0;
        return;
    }
    state->p25_frame_valid = 1;
    state->p25_frame_duid = (uint8_t)((duid[0] - '0') * 4 + duid[1] - '0');

    /* a talkgroup change means we are on a new call, so the ALGID
     * from the previous LDU2 no longer applies. Snapshot before dispatch;
     * if the frame writes a new non-zero lasttg, clear the ESS. */
    int pre_tg = state->lasttg;

    if (strcmp(duid, "00") == 0) {
        diag_count_frame("00");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" HDU\n"); }
        mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
        /* HDU begins a new call. Its parser may replace this unknown ESS
         * only after the header passes FEC. */
        p25_ess_clear(state);
        state->lastp25type = 2;
        sprintf(state->fsubtype, " HDU          ");
        processHDU(opts, state);
    } else if (strcmp(duid, "11") == 0) {
        diag_count_frame("11");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" LDU1  "); }
        state->lastp25type = 1;
        sprintf(state->fsubtype, " LDU1         ");
        state->numtdulc = 0;
        processLDU1(opts, state);
    } else if (strcmp(duid, "22") == 0) {
        diag_count_frame("22");
        if (state->lastp25type != 1) {
            if (opts->errorbars == 1) {
                printFrameInfo(opts, state);
                printf(" Ignoring LDU2 not preceeded by LDU1\n");
            }
            state->lastp25type = 0;
            sprintf(state->fsubtype, "              ");
        } else {
            if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" LDU2  "); }
            state->lastp25type = 2;
            sprintf(state->fsubtype, " LDU2         ");
            state->numtdulc = 0;
            processLDU2(opts, state);
        }
    } else if (strcmp(duid, "33") == 0) {
        diag_count_frame("33");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" TDULC\n"); }
        mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
        state->lasttg = 0;
        state->lastsrc = 0;
        state->lastp25type = 0;
        state->err_str[0] = 0;
        /* TDULC ends a call. Clear ESS so the next call starts
         * unknown - anything else lets a mute leak from one call to the
         * next until reboot. */
        p25_ess_clear(state);
        /* same for the LCW identity, or the next call would inherit
         * the previous emergency flag, alias and TG label. */
        p25_lcw_call_clear(state);
        sprintf(state->fsubtype, " TDULC        ");
        state->numtdulc++;
        processTDULC(opts, state);
        state->err_str[0] = 0;
    } else if (strcmp(duid, "03") == 0) {
        diag_count_frame("03");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" TDU\n"); }
        mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
        state->lasttg = 0;
        state->lastsrc = 0;
        state->lastp25type = 0;
        state->err_str[0] = 0;
        /* same as TDULC - end of call, drop the ESS. */
        p25_ess_clear(state);
        /* and the LCW identity, for the same reason. */
        p25_lcw_call_clear(state);
        sprintf(state->fsubtype, " TDU          ");
        processTDU(opts, state);
    } else if (strcmp(duid, "13") == 0) {
        diag_count_frame("13");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" TSDU\n"); }
        state->lasttg = 0;
        state->lastsrc = 0;
        state->lastp25type = 3;
        sprintf(state->fsubtype, " TSDU         ");
        captureTSDU(opts, state);
    } else if (strcmp(duid, "30") == 0) {
        diag_count_frame("30");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" PDU\n"); }
        state->lastp25type = 4;
        sprintf(state->fsubtype, " PDU          ");
    } else if (state->lastp25type == 1) {
        /* a validated but unsupported DUID is not an inferred LDU. */
        state->p25_frame_valid = 0;
        state->pcm_out_write = 0;
    } else if (state->lastp25type == 2) {
        state->p25_frame_valid = 0;
        state->pcm_out_write = 0;
    } else if (state->lastp25type == 3) {
        diag_count_frame("13");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" (TSDU)\n"); }
        state->lastp25type = 3;
        sprintf(state->fsubtype, "(TSDU)        ");
        captureTSDU(opts, state);
    } else if (state->lastp25type == 4) {
        diag_count_frame("30");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" (PDU)\n"); }
        state->lastp25type = 0;
    } else {
        diag_count_frame("??");
        state->lastp25type = 0;
        sprintf(state->fsubtype, "              ");
        if (opts->errorbars == 1) {
            printFrameInfo(opts, state);
            printf(" duid:%s *Unknown DUID*\n", duid);
        }
    }

    /* if the frame we just dispatched landed a different talkgroup,
     * the ESS from the prior call no longer applies. Comparing after the
     * dispatch catches HDU (talkgroup set in processHDU) and LDU1 (talkgroup
     * set inside the LC parser); TDU/TDULC already cleared above. */
    if (state->lasttg != 0 && pre_tg != 0 && state->lasttg != pre_tg) {
        /* HDU already retired the prior ESS before publishing its own.
         * Do not erase that freshly validated header on a TG change. */
        if (strcmp(duid, "00") != 0) p25_ess_clear(state);
        /* LCW here is the NEW call, not the old one. Clearing it
         * erased validated identity and hid a mismatch from the follower.
         * Retire old PCM; the LCW parser retires aliases before replacing
         * identity, so the new group/source remains available to compare. */
        state->pcm_out_write = 0;
    }
}
