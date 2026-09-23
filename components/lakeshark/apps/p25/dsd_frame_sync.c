/*
 * DSD-derived source, modified for LakeShark.
 * Comparison source: https://github.com/szechyjs/dsd/blob/59423fa46be8b41ef0bd2f3d2b45590600be29f0/src/dsd_frame_sync.c
 * Original import revision is unrecorded.
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

/* Anything that wants the raw symbol stream attaches here. The demodulator
   does not know what is listening, which keeps the P25 sources free of a
   link dependency on every decoder that might want a copy. */
static dsd_symbol_observer_t s_symbol_observer;

void dsd_set_symbol_observer(dsd_symbol_observer_t fn) { s_symbol_observer = fn; }

/* How many of the 24 sync symbols may disagree with the pattern.

   The pattern is 24 outer symbols; a real sync is still recognised with a few
   of them wrong, and a false one is caught next by the NID's BCH check in
   processFrame, which rejects it before anything is decoded. Exact match was
   upstream's rule and it lost the start of every transmission on 154.7850
   (2026-09-23 capture): the first LDU1 of each call arrived with 2-3 symbols
   off while the demodulator settled, and was hunted past. On that capture 0
   errors found 6 of 9 LDU1s, 3 found all 9; 4 began locking on a false sync
   and cost a real frame. In noise, 3 errors pass about 1.4e-4 of positions -
   under one false sync a second, each costing one NID read. */
#define P25_SYNC_MAX_ERRORS 3

/* Symbols into a hunt after which it counts as acquiring. The flywheel sync
   between back-to-back frames lands at 24. */
#define P25_TIMING_ACQUIRE_AFTER 30
/* Frames found back to back before the symbol clock turns patient. */
#define P25_TIMING_LOCK_RUN 2

/* A sync was found t symbols into the hunt: back to back with the frame
   before it, or not. */
static void timing_sync(dsd_state *state, int t)
{
    state->c4fm_timing_run = t <= P25_TIMING_ACQUIRE_AFTER ? state->c4fm_timing_run + 1 : 1;
}

/* Mismatches between the last 24 symbols and a sync pattern, each symbol cut
   at the window's own midpoint. A sync holds only outer symbols, so the
   window's extremes ARE the two outer levels and their midpoint is the true
   centre - where slicing at zero is wrong by whatever DC the discriminator
   still carries. At carrier onset that was ~1100 against ~2200 outer levels,
   for the first 100 ms of every call: the HDU was on the air, clean, and
   failed the old zero-threshold test. */
static int sync_errors(const int *ring, int oldest, int centre, const char *pattern)
{
    int errors = 0;
    for (int k = 0; k < 24; k++) {
        const char want = pattern[k];
        const char got = ring[(oldest + k) % 24] > centre ? '1' : '3';
        if (got != want) errors++;
    }
    return errors;
}

static void publish_hunt(dsd_state *state, int low, int high,
                         unsigned normal_hd, unsigned inverted_hd)
{
    state->acquisition_hunt.symbol_min = low;
    state->acquisition_hunt.symbol_max = high;
    state->acquisition_hunt.best_normal_hd = normal_hd;
    state->acquisition_hunt.best_inverted_hd = inverted_hd;
}

void
printFrameSync(dsd_opts *opts, dsd_state *state, char *frametype, int offset, char *modulation)
{
    if (opts->verbose > 0)
        printf("Sync: %s ", frametype);
    if (opts->verbose > 2)
        printf("o: %4i ", offset);
    if (opts->verbose > 1)
        printf("mod: %s ", modulation);
}

int
getFrameSync(dsd_opts *opts, dsd_state *state)
{
    state->acquisition_hunt.attempts++;
    int i, t, o, dibit, sync, symbol, synctest_pos, lastt;
    char synctest[25];
    char modulation[8];
    char *synctest_p;
    char synctest_buf[10240];
    int lmin, lmax, lidx;
    int lbuf[24], lbuf2[24];
    int lsum;

    for (i = 18; i < 24; i++) {
        lbuf[i] = 0;
        lbuf2[i] = 0;
    }

    t = 0;
    synctest[24] = 0;
    synctest_pos = 0;
    synctest_p = synctest_buf + 10;
    sync = 0;
    lmin = 0;
    lmax = 0;
    lidx = 0;
    lastt = 0;
    state->numflips = 0;

    int diag_symmin =  32767;
    int diag_symmax = -32768;
    int diag_rf_mod_observed = -1;

    int diag_best_hd_norm     = 99;
    int diag_best_hd_norm_pos = -1;
    int diag_best_hd_inv      = 99;
    int diag_best_hd_inv_pos  = -1;

    while (sync == 0) {
        if (exitflag == 1)
            return -1;

        t++;
        /* Between two frames the next sync is at symbol 24 of this hunt;
           a hunt that has gone past that is acquiring, and its symbol clock
           must move at every crossing (getSymbol). So is one that has not
           yet found two frames back to back: a transmitter on 154.7850 opens
           with a 72-symbol frame and then its HDU, so the HDU arrives on
           time after only 15 ms of carrier - too soon for a patient clock,
           which found 1 of its 3 HDUs where an eager one found 3. */
        state->c4fm_timing_acquiring = t > P25_TIMING_ACQUIRE_AFTER ||
                                       state->c4fm_timing_run < P25_TIMING_LOCK_RUN;
        symbol = getSymbol(opts, state, 0);
        state->acquisition_hunt.symbols++;

        /* Every symbol on the channel passes here while the P25 sync search
           runs, and is thrown away when it is not P25. An observer sees the
           same symbols against the same slicer thresholds. Only once those
           have been measured - they are zero for the first 18 symbols - and
           only in C4FM, which is the 4FSK the other modes on this rate use. */
        if (s_symbol_observer && state->rf_mod == 0 &&
            state->umid > state->center && state->center > state->lmid)
            s_symbol_observer(symbol, state->center, state->umid, state->lmid);

        if (symbol < diag_symmin) diag_symmin = symbol;
        if (symbol > diag_symmax) diag_symmax = symbol;

        lbuf[lidx] = symbol;
        state->sbuf[state->sidx] = symbol;
        lidx = (lidx == 23) ? 0 : lidx + 1;
        state->sidx = (state->sidx == (opts->ssize - 1)) ? 0 : state->sidx + 1;

        if (lastt == 23) {
            lastt = 0;
            if (state->numflips > opts->mod_threshold) {
                if (opts->mod_qpsk == 1)
                    state->rf_mod = 1;
            } else if (state->numflips > 18) {
                if (opts->mod_gfsk == 1)
                    state->rf_mod = 2;
            } else {
                if (opts->mod_c4fm == 1)
                    state->rf_mod = 0;
            }
            diag_rf_mod_observed = state->rf_mod;
            state->numflips = 0;
        } else {
            lastt++;
        }

        if (state->dibit_buf_p > state->dibit_buf + 9000)
            state->dibit_buf_p = state->dibit_buf + 200;

        if (symbol > 0) {
            *state->dibit_buf_p = 1;
            state->dibit_buf_p++;
            dibit = 49;
        } else {
            *state->dibit_buf_p = 3;
            state->dibit_buf_p++;
            dibit = 51;
        }

        *synctest_p = dibit;

        if (t >= 18) {
            for (i = 0; i < 24; i++)
                lbuf2[i] = lbuf[i];
            qsort(lbuf2, 24, sizeof(int), comp);
            lmin = (lbuf2[2] + lbuf2[3] + lbuf2[4]) / 3;
            lmax = (lbuf2[21] + lbuf2[20] + lbuf2[19]) / 3;

            if (state->rf_mod == 0 || state->rf_mod == 1) {
                state->minbuf[state->midx] = lmin;
                state->maxbuf[state->midx] = lmax;
                state->midx = (state->midx == (opts->msize - 1)) ? 0 : state->midx + 1;
                lsum = 0;
                for (i = 0; i < opts->msize; i++)
                    lsum += state->minbuf[i];
                state->min = lsum / opts->msize;
                lsum = 0;
                for (i = 0; i < opts->msize; i++)
                    lsum += state->maxbuf[i];
                state->max = lsum / opts->msize;
                state->center = ((state->max) + (state->min)) / 2;
                state->maxref = (int)((state->max) * 0.80F);
                state->minref = (int)((state->min) * 0.80F);

                state->umid = (((state->max) - state->center) / 2) + state->center;
                state->lmid = (((state->min) - state->center) / 2) + state->center;
            } else {
                state->maxref = state->max;
                state->minref = state->min;
            }

            if (state->rf_mod == 0)
                sprintf(modulation, "C4FM");
            else if (state->rf_mod == 1)
                sprintf(modulation, "QPSK");
            else if (state->rf_mod == 2)
                sprintf(modulation, "GFSK");

            strncpy(synctest, (synctest_p - 23), 24);

            int hd_norm = 24, hd_inv = 24;
            /* A full window only: before 24 symbols part of it is the
               zeroed start of lbuf. lidx is the oldest entry now. */
            if (opts->frame_p25p1 == 1 && t >= 24) {
                const int local_centre = (lmax + lmin) / 2;
                hd_norm = sync_errors(lbuf, lidx, local_centre, P25P1_SYNC);
                hd_inv  = sync_errors(lbuf, lidx, local_centre, INV_P25P1_SYNC);
                if (hd_norm < diag_best_hd_norm) {
                    diag_best_hd_norm = hd_norm;
                    diag_best_hd_norm_pos = synctest_pos;
                }
                if (hd_inv < diag_best_hd_inv) {
                    diag_best_hd_inv = hd_inv;
                    diag_best_hd_inv_pos = synctest_pos;
                }
            }

            if (opts->frame_p25p1 == 1) {
                if (hd_norm <= P25_SYNC_MAX_ERRORS) {
                    state->acquisition_hunt.raw_syncs++;
                    publish_hunt(state, diag_symmin, diag_symmax,
                                 diag_best_hd_norm, diag_best_hd_inv);
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    /* streamed clean IQ found sync but sliced its
                     * first NID as NAC 0x282 instead of 0x293. The 256-entry
                     * history still held startup +/-15000, dwarfing this
                     * burst's outer symbols. A complete P25 sync contains
                     * only outer symbols: seed tracking from that evidence
                     * so getDibit cannot restore the stale thresholds. */
                    state->max = lmax;
                    state->min = lmin;
                    for (int k = 0; k < opts->msize; k++) {
                        state->maxbuf[k] = lmax;
                        state->minbuf[k] = lmin;
                    }

                    state->center = ((state->max) + (state->min)) / 2;
                    state->umid = (((state->max) - state->center) / 2) + state->center;
                    state->lmid = (((state->min) - state->center) / 2) + state->center;
                    sprintf(state->ftype, " P25 Phase 1 ");
                    if (opts->errorbars == 1)
                        printFrameSync(opts, state, " +P25p1    ", synctest_pos + 1, modulation);
                    state->lastsynctype = 0;
                    state->synctype     = 0;

                    diag_line("SYN_HIT", "P25p1 pos=%d t=%d", synctest_pos, t);
                    diag_line("SLICE", "lock min=%d max=%d center=%d lmid=%d umid=%d lmax_in=%d lmin_in=%d",
                              state->min, state->max, state->center,
                              state->lmid, state->umid, lmax, lmin);
                    timing_sync(state, t);
                    return 0;
                }
                if (hd_inv <= P25_SYNC_MAX_ERRORS) {
                    state->acquisition_hunt.inverted_matches++;
                    publish_hunt(state, diag_symmin, diag_symmax,
                                 diag_best_hd_norm, diag_best_hd_inv);

                }
            }

            if ((t == 24) && (state->lastsynctype != -1)) {
                if ((state->lastsynctype == 0) && ((state->lastp25type == 1) || (state->lastp25type == 2))) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + (lmax)) / 2;
                    state->min = ((state->min) + (lmin)) / 2;
                    state->center = ((state->max) + (state->min)) / 2;
                    state->umid = (((state->max) - state->center) / 2) + state->center;
                    state->lmid = (((state->min) - state->center) / 2) + state->center;
                    sprintf(state->ftype, "(P25 Phase 1)");
                    if (opts->errorbars == 1)
                        printFrameSync(opts, state, "(+P25p1)   ", synctest_pos + 1, modulation);
                    state->lastsynctype = -1;
                    state->synctype     = 0;
                    timing_sync(state, t);
                    return 0;
                } else if ((state->lastsynctype == 1) && ((state->lastp25type == 1) || (state->lastp25type == 2))) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    state->center = ((state->max) + (state->min)) / 2;
                    state->umid = (((state->max) - state->center) / 2) + state->center;
                    state->lmid = (((state->min) - state->center) / 2) + state->center;
                    sprintf(state->ftype, "(P25 Phase 1)");
                    if (opts->errorbars == 1)
                        printFrameSync(opts, state, "(-P25p1)   ", synctest_pos + 1, modulation);
                    state->lastsynctype = -1;
                    timing_sync(state, t);
                    return 1;
                }
            }
        }

        if (synctest_pos < 10200) {
            synctest_pos++;
            synctest_p++;
        } else {
            synctest_pos = 0;
            synctest_p = synctest_buf;
            noCarrier(opts, state);
        }

        if (synctest_pos >= 1800) {
            state->acquisition_hunt.timeouts++;
            publish_hunt(state, diag_symmin, diag_symmax,
                         diag_best_hd_norm, diag_best_hd_inv);
            static int hunt_log_counter = 0;
            hunt_log_counter++;

            if ((hunt_log_counter % 20) == 0) {
                diag_line("HUNT", "n=%d t=%d rf_mod=%d symmin=%d symmax=%d center=%d lmid=%d umid=%d thr_min=%d thr_max=%d",
                          hunt_log_counter, t, diag_rf_mod_observed,
                          diag_symmin, diag_symmax,
                          state->center, state->lmid, state->umid,
                          state->min, state->max);

                char win[25];
                if (synctest_pos >= 24) {

                    char *src = synctest_p - 23;

                    if (src >= synctest_buf && src + 24 <= synctest_buf + 10240) {
                        for (int k = 0; k < 24; k++) win[k] = src[k];
                        win[24] = 0;
                        diag_line("HUNT", "win=[%s] expect=[111113113311333313133333]", win);
                    }
                }

                if (synctest_pos >= 24) {
                    char inv[25];
                    char *src = synctest_p - 23;
                    if (src >= synctest_buf && src + 24 <= synctest_buf + 10240) {
                        for (int k = 0; k < 24; k++) inv[k] = (src[k] == '1') ? '3' : '1';
                        inv[24] = 0;
                        diag_line("HUNT", "inv=[%s] (if == expect, we have polarity flip)", inv);
                    }
                }

                int count1 = 0, count3 = 0;
                for (int k = 0; k < 1800; k++) {
                    char c = synctest_buf[k];
                    if (c == '1') count1++;
                    else if (c == '3') count3++;
                }
                diag_line("HUNT", "dibit_hist ones=%d threes=%d (balanced ~50/50 for P25)",
                          count1, count3);

                diag_line("SHD", "best_hd_norm=%d@pos=%d best_hd_inv=%d@pos=%d",
                          diag_best_hd_norm, diag_best_hd_norm_pos,
                          diag_best_hd_inv,  diag_best_hd_inv_pos);
                diag_best_hd_norm = 99; diag_best_hd_norm_pos = -1;
                diag_best_hd_inv  = 99; diag_best_hd_inv_pos  = -1;
            }
            noCarrier(opts, state);
            return -1;
        }
    }

    return -1;
}
