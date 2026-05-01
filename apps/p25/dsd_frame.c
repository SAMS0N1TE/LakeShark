/*
 * dsd_frame.c — Phase 1 replacement.
 *
 * Changes from the previous version:
 *   • Removed Session 8/9/10 "flight recorder" calls (dsd_timing_trace_*,
 *     dsd_raw_trace_*). They stay referenced in dsd_symbol.c as weak
 *     no-ops if you want to keep them for later, but dsd_frame.c no
 *     longer drives them.
 *   • Replaced sys_log() with diag_line() so telemetry goes out UART1.
 *   • Simplified BCH-OK / BCH-FAIL branches: one compact line each
 *     via diag_count_bch_result(), plus periodic dumps at 1/10 rate to
 *     keep UART traffic under control.
 *   • Removed the 60% slicer-threshold tweak and the "Session 10"
 *     commentary — those were compensating for a broken decoder.
 *     Threshold math is back to the textbook 50%.
 *   • Kept the DUID-valid-fallback logic (if BCH fails but raw DUID
 *     looks legal, process the frame anyway) — useful for recovering
 *     audio when one bit flipped in the NID.
 */
#include "dsd.h"
#include "diag.h"
#if !defined(NULL)
#define NULL 0
#endif

/* Forward decl — full API in audio_hal.h. Declared here rather than
 * including audio_hal.h so this TU stays portable for offline MinGW
 * validation (audio_hal.h transitively pulls in esp_err.h). Must match
 * the enum value BEEP_SYNC_SUCCESS = 1 in audio_hal.h. */
#ifndef AUDIO_HAL_H
typedef int beep_kind_t;
extern void audio_beep_request(beep_kind_t kind);
#define BEEP_SYNC_SUCCESS ((beep_kind_t)1)
#endif

#include "p25p1_check_nid.h"

/* Counters used by class_driver.c's autoscan and S.dsd_bch_ok_count export.
 * Defined in class_driver.c; we just reference them here. */
extern int autoscan_bch_ok_flag;
extern int dsd_bch_fail_counter;

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

    /* Dibit buffer used by DUMP diag on BCH FAIL. */
    int nid_dibits[33] = {0};

    nac[12] = 0;
    duid[2] = 0;
    j = 0;

    /* Slicer thresholds recomputed from fresh min/max (classic 50% boundary).
     * The previous 60% tweak was working around broken BCH; with the new
     * decoder, 50% is fine and the heuristics in dsd_dibit.c will refine
     * from there. */
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

    /* NAC: 6 dibits → 12 bits */
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

    /* DUID: 2 dibits → 4 bits */
    for (i = 0; i < 2; i++) {
        dibit = getDibit(opts, state);
        nid_dibits[6 + i] = dibit;
        duid[i] = dibit + '0';
        bch_code[index_bch_code++] = 1 & (dibit >> 1);
        bch_code[index_bch_code++] = 1 & dibit;
    }

    /* First 3 dibits of parity (6 bits) */
    for (i = 0; i < 3; i++) {
        dibit = getDibit(opts, state);
        nid_dibits[8 + i] = dibit;
        bch_code[index_bch_code++] = 1 & (dibit >> 1);
        bch_code[index_bch_code++] = 1 & dibit;
    }

    /* Status symbol (discarded) */
    status_0 = getDibit(opts, state) + '0';
    (void)status_0;

    /* Remaining 20 dibits of parity (40 bits) */
    for (i = 0; i < 20; i++) {
        dibit = getDibit(opts, state);
        nid_dibits[11 + i] = dibit;
        bch_code[index_bch_code++] = 1 & (dibit >> 1);
        bch_code[index_bch_code++] = 1 & dibit;
    }

    /* Last NID dibit: high bit = bch_code[62], low bit = parity */
    dibit = getDibit(opts, state);
    nid_dibits[31] = dibit;
    bch_code[index_bch_code] = 1 & (dibit >> 1);
    parity = (1 & dibit);
    nid_dibits[32] = parity;

    /* ── BCH decode ── */
    check_result = check_NID_ec(bch_code, &new_nac, new_duid, parity, &bch_ec);

    diag_count_bch_result(check_result, bch_ec);

    if (check_result) {
        autoscan_bch_ok_flag++;
        /* Fire-and-forget through the beep dispatcher. Returns
         * immediately. Will be no-op if:
         *   - S.sync_beep_enabled is false (default — toggle [B] on
         *     settings page)
         *   - voice audio is currently streaming
         *   - another beep is already in flight
         * This replaces the old synchronous audio_play_success() call
         * that stalled the decode thread ~240ms per success and stomped
         * the same I2S channel used by voice. */
        audio_beep_request(BEEP_SYNC_SUCCESS);

        /* Every 10th success: dump the NID dibits for offline analysis. */
        static int ok_ctr = 0;
        if ((++ok_ctr % 10) == 0) {
            diag_dump_nid("OKDMP", nid_dibits, new_nac, new_duid, bch_ec, 1, NULL);
        }
    } else {
        dsd_bch_fail_counter++;
        /* Every 4th failure: dump so we can inspect the symbol pattern. */
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
        /* Raw-DUID fallback: if BCH failed but the DUID value looks legal,
         * keep going. The NAC is the first 12 bits which tend to be
         * clean; a flipped parity bit shouldn't kill a whole voice frame. */
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

    /* Per-frame dispatch (unchanged from original structure). */
    if (strcmp(duid, "00") == 0) {
        diag_count_frame("00");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" HDU\n"); }
        mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
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
        sprintf(state->fsubtype, " TDU          ");
        processTDU(opts, state);
    } else if (strcmp(duid, "13") == 0) {
        diag_count_frame("13");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" TSDU\n"); }
        state->lasttg = 0;
        state->lastsrc = 0;
        state->lastp25type = 3;
        sprintf(state->fsubtype, " TSDU         ");
        skipDibit(opts, state, 328 - 25);
    } else if (strcmp(duid, "30") == 0) {
        diag_count_frame("30");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" PDU\n"); }
        state->lastp25type = 4;
        sprintf(state->fsubtype, " PDU          ");
    } else if (state->lastp25type == 1) {
        diag_count_frame("22");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf("(LDU2) "); }
        state->lastp25type = 2;
        sprintf(state->fsubtype, "(LDU2)        ");
        state->numtdulc = 0;
        processLDU2(opts, state);
    } else if (state->lastp25type == 2) {
        diag_count_frame("11");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf("(LDU1) "); }
        state->lastp25type = 1;
        sprintf(state->fsubtype, "(LDU1)        ");
        state->numtdulc = 0;
        processLDU1(opts, state);
    } else if (state->lastp25type == 3) {
        diag_count_frame("13");
        if (opts->errorbars == 1) { printFrameInfo(opts, state); printf(" (TSDU)\n"); }
        state->lastp25type = 3;
        sprintf(state->fsubtype, "(TSDU)        ");
        skipDibit(opts, state, 328 - 25);
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
}
