#include "dsd.h"
#include "diag.h"

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
        symbol = getSymbol(opts, state, 0);

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
            if (opts->frame_p25p1 == 1) {
                hd_norm = 0; hd_inv = 0;
                for (int k = 0; k < 24; k++) {
                    if (synctest[k] != P25P1_SYNC[k])     hd_norm++;
                    if (synctest[k] != INV_P25P1_SYNC[k]) hd_inv++;
                }
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
                if (strcmp(synctest, P25P1_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;

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
                    return 0;
                }
                if (strcmp(synctest, INV_P25P1_SYNC) == 0) {

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
