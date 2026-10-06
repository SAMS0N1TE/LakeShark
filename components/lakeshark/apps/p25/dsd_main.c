/*
 * DSD-derived source, modified for LakeShark.
 * Comparison source: https://github.com/szechyjs/dsd/blob/59423fa46be8b41ef0bd2f3d2b45590600be29f0/src/dsd_main.c
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
#include "esp_log.h"
#include "esp_heap_caps.h"

#define USE_OP25_VOCODER 1
#include "imbe_shim.h"
#include "imbe_chase.h"

volatile int p25_voice_gate = 99;

#define DSD_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM)

static inline void *dsd_malloc_fast(size_t sz)
{

    /* Leave DMA-capable memory available for Bluetooth and USB app switches. */
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > sz + 24576 &&
        heap_caps_get_free_size(dma_caps) > sz + 8192 &&
        heap_caps_get_largest_free_block(dma_caps) > sz + 4096) {
        void *p = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) return p;
    }
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
}
#define DSD_MALLOC_FAST(sz) dsd_malloc_fast((sz))

static const char *TAG_DSD = "DSD_INIT";

int exitflag = 0;
volatile int dsd_abort = 0;

int comp(const void *a, const void *b)
{
    if (*((const int *)a) == *((const int *)b))
        return 0;
    else if (*((const int *)a) < *((const int *)b))
        return -1;
    else
        return 1;
}

void initOpts(dsd_opts *opts)
{
    opts->errorbars = 0;
    opts->verbose = 2;
    opts->p25enc = 0;
    opts->p25lc = 0;
    opts->p25status = 0;
    opts->p25tg = 0;
    opts->frame_p25p1 = 1;
    opts->mod_c4fm = 1;
    opts->mod_qpsk = 1;
    opts->mod_gfsk = 1;
    opts->uvquality = 3;
    opts->mod_threshold = 26;
    opts->ssize = 36;
    opts->msize = 256;
    opts->use_cosine_filter = 1;
    opts->unmute_encrypted_p25 = 0;
    opts->play_unproven = 0;
    opts->p25_nid_repair = 1;
    opts->erasure_marks = 0;
    opts->soft_symbols = 0;
    opts->imbe_repeat = 1;
    opts->audio_gain = 0.0f;
    opts->audio_out = 1;
    opts->symboltiming = 0;
    opts->datascope = 0;
    opts->scoperate = 15;
    opts->ring = NULL;
}

#define DSD_KEEP(field, type, count, how)                                        \
    do {                                                                         \
        if (!state->field) {                                                     \
            state->field = (type *)how(sizeof(type) * (count));                  \
            ESP_LOGI(TAG_DSD, #field ": %p (%d bytes)", (void *)state->field,    \
                     (int)(sizeof(type) * (count)));                             \
        }                                                                        \
    } while (0)

void initState(dsd_state *state)
{
    int i;
    memset(&state->acquisition_hunt, 0, sizeof(state->acquisition_hunt));
    DSD_KEEP(dibit_buf, int, 10000, DSD_MALLOC);
    if (!state->dibit_buf) { ESP_LOGE(TAG_DSD, "dibit_buf alloc FAILED"); return; }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->repeat = 0;
    DSD_KEEP(audio_out_buf, short, 2000, DSD_MALLOC_FAST);
    if (!state->audio_out_buf) { ESP_LOGE(TAG_DSD, "audio_out_buf alloc FAILED"); return; }
    state->audio_out_buf_p = state->audio_out_buf;
    DSD_KEEP(audio_out_float_buf, float, 2000, DSD_MALLOC_FAST);
    if (!state->audio_out_float_buf) { ESP_LOGE(TAG_DSD, "float_buf alloc FAILED"); return; }
    state->audio_out_float_buf_p = state->audio_out_float_buf;
    state->audio_out_temp_buf_p = state->audio_out_temp_buf;
    state->audio_out_idx = 0;
    state->audio_out_idx2 = 0;
    state->center = 0;
    state->jitter = -1;
    state->synctype = -1;
    state->min = -15000;
    state->max = 15000;
    state->lmid = 0;
    state->umid = 0;
    state->minref = state->min;
    state->maxref = state->max;
    state->lastsample = 0;
    for (i = 0; i < 128; i++)
        state->sbuf[i] = 0;
    state->sidx = 0;
    for (i = 0; i < 1024; i++) {
        state->maxbuf[i] = 15000;
        state->minbuf[i] = -15000;
    }
    state->midx = 0;
    state->err_str[0] = 0;
    sprintf(state->fsubtype, "              ");
    sprintf(state->ftype, "              ");
    state->symbolcnt = 0;
    state->rf_mod = 0;
    state->numflips = 0;
    state->lastsynctype = -1;
    state->lastp25type = 0;
    state->offset = 0;
    state->carrier = 0;
    for (i = 0; i < 25; i++)
        state->tg[i][0] = 0;
    state->tgcount = 0;
    state->lasttg = 0;
    state->lastsrc = 0;
    state->nac = 0;
    state->errs = 0;
    state->errs2 = 0;
    state->optind = 0;
    state->numtdulc = 0;
    state->firstframe = 0;
    sprintf(state->slot0light, " slot0 ");
    sprintf(state->slot1light, " slot1 ");
    state->aout_gain = 25.0f;
    state->aout_max_buf_p = state->aout_max_buf;
    state->aout_max_buf_idx = 0;
    for (i = 0; i < 200; i++)
        state->aout_max_buf[i] = 0.0f;

    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;

    state->c4fm_clk_mode = 2;
    state->c4fm_clk_prev_dec = 0;
    state->c4fm_clk_run_dir = 0;
    state->c4fm_clk_run_len = 0;
    state->c4fm_clk_cooldown = 0;
    state->c4fm_timing_acc = 0;
    state->c4fm_timing_acquiring = 1;
    state->c4fm_timing_run = 0;
    state->c4fm_clk_nudges = 0;
    memset(state->algid, 0, 9);
    memset(state->keyid, 0, 17);
    state->p25_algid = 0;
    state->p25_kid = 0;
    memset(state->p25_mi, 0, sizeof(state->p25_mi));
    state->p25_ess_valid = 0;
    state->p25_ess_pending = 0;
    state->p25_ess_doubted = 0;
    state->currentslot = 0;
    DSD_KEEP(cur_mp, mbe_parms, 1, DSD_MALLOC_FAST);
    DSD_KEEP(prev_mp, mbe_parms, 1, DSD_MALLOC_FAST);
    DSD_KEEP(prev_mp_enhanced, mbe_parms, 1, DSD_MALLOC_FAST);

    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    state->p25kid = 0;
    state->debug_audio_errors = 0;
    state->debug_header_errors = 0;
    state->debug_header_critical_errors = 0;
    state->last_dibit = 0;
    state->p25_tsdu_dibit_count = 0;
    memset(state->p25_iden_table, 0, sizeof(state->p25_iden_table));
    state->p25_tsbk_last_opcode = 0;
    state->p25_tsbk_channel = 0;
    state->p25_tsbk_talkgroup = 0;
    state->p25_tsbk_source = 0;
    state->p25_tsbk_frequency_hz = 0;
    memset(state->p25_grants, 0, sizeof(state->p25_grants));
    state->p25_grant_count = 0;
    state->p25_grant_batch_valid = 0;
    state->p25_grant_generation = 0;
    state->p25_frame_valid = 0;
    state->p25_frame_duid = 0;
    state->p25_good_nac = -1;
    state->imbe_error_rate = 0.0f;
    state->imbe_have_good = 0;
    state->imbe_repeats_in_row = 0;
    state->imbe_repeated = state->imbe_muted = 0;
    state->imbe_gated = 0;
    state->imbe_frames = state->imbe_clean = state->imbe_rough = 0;
    state->erased_dibits = 0;
    state->imbe_frame_erased = 0;
    state->imbe_erased = 0;
    state->p25_nid_repaired = 0;
    state->p25_nid_d1 = state->p25_nid_d2 = -1;
    state->p25_frame_tsbks = 0;
    state->p25_control_nac = 0;
    state->p25_control_nac_valid = 0;
    state->p25_phase2_grant_count = 0;
    state->p25_phase2_last_talkgroup = 0;
    state->p25_phase2_last_frequency_hz = 0;
    state->p25_phase2_last_slot = 0;
    state->p25_phase2_last_slots_per_carrier = 0;
    state->p25_tsbk_wacn = 0;
    state->p25_tsbk_sysid = 0;
    state->p25_net_valid = 0;
    state->p25_net_generation = 0;
    state->p25_rfss_generation = 0;
    state->p25_sccb_generation = 0;
    state->p25_neighbor_generation = 0;
    state->p25_tsbk_valid_count = 0;
    state->p25_tsbk_crc_errors = 0;
    state->p25_tsbk_trellis_errors = 0;
    state->p25_tsbk_vendor_count = 0;
    state->p25_tsbk_last_vendor_mfid = 0;
    /* unhandled-opcode counters and per-broadcast state added by
     * the wider TSBK dispatch. All zero on entry: neither RFSS nor SCCB
     * nor SYS_SRV nor SYNC nor any neighbour has been seen yet. */
    memset(state->p25_tsbk_unhandled, 0, sizeof(state->p25_tsbk_unhandled));
    state->p25_rfss_valid = 0;
    state->p25_rfss_lra = 0;
    state->p25_rfss_id = 0;
    state->p25_rfss_site_id = 0;
    state->p25_rfss_sysid = 0;
    state->p25_rfss_ch_t = 0;
    state->p25_rfss_ch_r = 0;
    state->p25_rfss_svc_class = 0;
    state->p25_sccb_valid = 0;
    state->p25_sccb_rfss_id = 0;
    state->p25_sccb_site_id = 0;
    state->p25_sccb_ch1 = 0;
    state->p25_sccb_ch2 = 0;
    state->p25_sccb_svc_class1 = 0;
    state->p25_sccb_svc_class2 = 0;
    state->p25_sys_srv_valid = 0;
    state->p25_sys_srv_available = 0;
    state->p25_sys_srv_supported = 0;
    state->p25_sys_srv_twv = 0;
    state->p25_sync_valid = 0;
    state->p25_sync_microslot = 0;
    state->p25_sync_us = 0;
    state->p25_sync_month_day = 0;
    state->p25_sync_year = 0;
    memset(state->p25_neighbors, 0, sizeof(state->p25_neighbors));
    state->p25_neighbor_count = 0;
    state->p25_data_grant_count = 0;
    state->p25_last_data_channel = 0;
    state->p25_last_data_group = 0;
    state->p25_interconnect_grant_count = 0;
    state->p25_last_interconnect_channel = 0;
    state->p25_last_interconnect_address = 0;
    state->p25_uu_grant_count = 0;
    state->p25_last_uu_channel = 0;
    state->p25_last_uu_source = 0;
    state->p25_last_uu_target = 0;
    state->p25_reg_event_count = 0;
    state->p25_last_reg_opcode = 0;
    state->p25_last_reg_reason = 0;
    state->p25_last_reg_source = 0;
    state->p25_last_reg_target = 0;
    /* zero the LCW state alongside TSBK state. p25_lcw_call_clear
     * does the same reset used on TDU/TDULC/talkgroup change. */
    state->p25_lcw_ok_count = 0;
    state->p25_lcw_fec_reject_count = 0;
    p25_lcw_call_clear(state);
    initialize_p25_heuristics(&state->p25_heuristics);
    initialize_p25_heuristics(&state->inv_p25_heuristics);
    state->pcm_out_buf = NULL;
    state->pcm_out_write = 0;
    state->pcm_out_size = 0;
}

void noCarrier(dsd_opts *opts, dsd_state *state)
{
    state->p25_frame_valid = 0;
    state->p25_frame_tsbks = 0;
    state->pcm_out_write = 0;
    state->dibit_buf_p = state->dibit_buf + 200;
    memset(state->dibit_buf, 0, sizeof(int) * 200);
    state->jitter = -1;
    state->lastsynctype = -1;
    state->carrier = 0;

    state->err_str[0] = 0;
    sprintf(state->fsubtype, "              ");
    state->errs = 0;
    state->errs2 = 0;
    state->lasttg = 0;
    state->lastsrc = 0;
    state->lastp25type = 0;
    state->repeat = 0;
    state->nac = 0;
    state->numtdulc = 0;
    sprintf(state->slot0light, " slot0 ");
    sprintf(state->slot1light, " slot1 ");
    state->firstframe = 0;
    state->aout_gain = 25.0f;
    state->aout_max_buf_idx = 0;
    for (int i = 0; i < 200; i++)
        state->aout_max_buf[i] = 0.0f;
    state->aout_max_buf_p = state->aout_max_buf;
    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    state->debug_audio_errors = 0;
    state->debug_header_errors = 0;
    state->debug_header_critical_errors = 0;
    state->p25_tsdu_dibit_count = 0;

    state->c4fm_clk_prev_dec = 0;
    state->c4fm_clk_run_dir = 0;
    state->c4fm_clk_run_len = 0;
    state->c4fm_clk_cooldown = 0;
    state->c4fm_timing_acc = 0;
    state->c4fm_timing_acquiring = 1;
    state->c4fm_timing_run = 0;
    p25_ess_clear(state);

    p25_lcw_call_clear(state);
}

void upsample(dsd_state *state, float invalue)
{
    int i, j;
    float sum;
    static const float upsample_coeffs[6] = {
        0.0f, 0.17f, 0.49f, 0.83f, 0.99f, 0.83f
    };

    state->audio_out_float_buf[state->audio_out_idx2] = invalue;
    state->audio_out_idx2++;
    state->audio_out_float_buf[state->audio_out_idx2] = 0.0f;
    state->audio_out_idx2++;
    state->audio_out_float_buf[state->audio_out_idx2] = 0.0f;
    state->audio_out_idx2++;
    state->audio_out_float_buf[state->audio_out_idx2] = 0.0f;
    state->audio_out_idx2++;
    state->audio_out_float_buf[state->audio_out_idx2] = 0.0f;
    state->audio_out_idx2++;
    state->audio_out_float_buf[state->audio_out_idx2] = 0.0f;
    state->audio_out_idx2++;
}

void processAudio(dsd_opts *opts, dsd_state *state)
{
    int i;
    float amax, *pamax;
    float gainfactor;

    amax = 0.0f;
    for (i = 0; i < 160; i++) {
        if (fabsf(state->audio_out_temp_buf[i]) > amax)
            amax = fabsf(state->audio_out_temp_buf[i]);
    }

    *state->aout_max_buf_p = amax;
    state->aout_max_buf_p++;
    state->aout_max_buf_idx++;
    if (state->aout_max_buf_idx > 24) {
        state->aout_max_buf_idx = 0;
        state->aout_max_buf_p = state->aout_max_buf;
    }

    amax = 0.0f;
    pamax = state->aout_max_buf;
    for (i = 0; i < 25; i++) {
        if (*pamax > amax)
            amax = *pamax;
        pamax++;
    }

    if (amax > 0.0f)
        gainfactor = (22937.0f / amax);
    else
        gainfactor = 50.0f;

    if (gainfactor > 200.0f) gainfactor = 200.0f;
    if (gainfactor < 1.0f)   gainfactor = 1.0f;

    if (gainfactor > state->aout_gain)
        gainfactor = state->aout_gain + ((gainfactor - state->aout_gain) * 0.1f);
    state->aout_gain = gainfactor;

    gainfactor = state->aout_gain;
    if (opts->audio_gain != 0.0f)
        gainfactor = opts->audio_gain;

    for (i = 0; i < 160; i++) {
        float sample = state->audio_out_temp_buf[i] * gainfactor;
        if (sample > 32760.0f)  sample = 32760.0f;
        if (sample < -32760.0f) sample = -32760.0f;

        if (state->pcm_out_buf && state->pcm_out_write < state->pcm_out_size) {
            state->pcm_out_buf[state->pcm_out_write++] = (short)sample;
        }

        if (state->audio_out_buf_p &&
            state->audio_out_idx < 1900) {
            state->audio_out_buf_p[0] = (short)sample;
            state->audio_out_buf_p++;
            state->audio_out_idx++;
        } else if (state->audio_out_buf) {

            state->audio_out_buf_p = state->audio_out_buf;
            state->audio_out_idx = 0;
        }
    }
}

void (*dsd_imbe_hook)(const uint8_t imbe88[11], int how, const dsd_state *state);

static void imbe_out(const uint8_t imbe88[11], int how, const dsd_state *state)
{
    if (dsd_imbe_hook) dsd_imbe_hook(imbe88, how, state);
}

void processMbeFrame(dsd_opts *opts, dsd_state *state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24])
{
    int i;
    char imbe_d[88];

    for (i = 0; i < 88; i++)
        imbe_d[i] = 0;

    int imbe_ones = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 23; c++)
            imbe_ones += (imbe_fr[r][c] & 1);

    if ((state->synctype == 0) || (state->synctype == 1)) {
#if USE_OP25_VOCODER

        int c0_sure_changed = 0;
        if (opts->soft_symbols) {
            /* each FEC word corrected with the source's doubts; mbelib's own
               pass then finds codewords and only takes the data out */
            state->errs = imbe_chase_c0(imbe_fr, state->imbe_doubt, &c0_sure_changed);
            (void)mbe_eccImbe7200x4400C0(imbe_fr);
            mbe_demodulateImbe7200x4400Data(imbe_fr);
            state->errs2 = imbe_chase_data(imbe_fr, state->imbe_doubt);
            (void)mbe_eccImbe7200x4400Data(imbe_fr, imbe_d);
        } else {
            state->errs  = mbe_eccImbe7200x4400C0(imbe_fr);
            mbe_demodulateImbe7200x4400Data(imbe_fr);
            state->errs2 = mbe_eccImbe7200x4400Data(imbe_fr, imbe_d);
        }
        state->debug_audio_errors += 0;
        uint8_t imbe88[11];
        for (int b = 0; b < 11; b++) imbe88[b] = 0;
        for (int b = 0; b < 88; b++)
            if (imbe_d[b] & 1) imbe88[b >> 3] |= (uint8_t)(0x80 >> (b & 7));
        int16_t snd[160];
        int how = DSD_IMBE_DECODED;
        /* TIA-102.BABA: E0 the errors fixed in u0, ET all of them, and a
           running rate. A frame with E0 >= 2 and ET >= 10 + 40*rate is
           replaced by the last good one, three times at most; past a rate of
           0.0875 the voice is muted. */
        const int et = state->errs + state->errs2;
        state->imbe_frames++;
        if (state->imbe_frame_erased) {
            /* Some of this frame's symbols were never received (the LR2021
               loses ~150 at each packet seam): what FEC makes of the rest is
               no frame at all. The last good one stands in, three times at
               most, else silence; the error rate is left alone. */
            state->imbe_erased++;
            if (state->imbe_have_good && state->imbe_repeats_in_row < 3) {
                state->imbe_repeats_in_row++;
                state->imbe_repeated++;
                for (int b = 0; b < 11; b++) imbe88[b] = state->imbe_last_good[b];
                how = DSD_IMBE_REPEATED;
                goto vocode;
            }
            state->imbe_muted++;
            for (int k = 0; k < 160; k++) state->audio_out_temp_buf[k] = 0.0f;
            state->err_str[0] = 0;
            imbe_out(imbe88, DSD_IMBE_MUTED, state);
            goto audio;
        }
        if (et <= 2) state->imbe_clean++;
        else if (et >= 6) state->imbe_rough++;
        state->imbe_error_rate = 0.95f * state->imbe_error_rate + 0.000365f * (float)et;
        if (opts->imbe_repeat && state->imbe_error_rate > 0.0875f) {
            state->imbe_muted++;
            for (int k = 0; k < 160; k++) state->audio_out_temp_buf[k] = 0.0f;
            state->err_str[0] = 0;
            imbe_out(imbe88, DSD_IMBE_MUTED, state);
            goto audio;
        }
        /* With the source's doubts, a c0 that had to change a bit the source
           was sure of is replaced too: scored against an RTL on a held-out
           call, that catches 39% of the wrong frames where the rule alone
           catches 32%, for 2.1% of the right ones where it costs 1.5%.
           So is a frame whose c1..c6 needed many fixes: the LR2021's 2400 Hz
           calls replayed through this decoder, errs2 >= 6 (>= 5 with two c0
           bits fixed) took out 96% of the frames that were garbage, over 10
           of 81 bits wrong, for 6% of the right ones, which a repeat of the
           frame before barely changes; garbage fell from 18.5% of the frames
           played to 0.9%. */
        const int many_fixed = opts->soft_symbols &&
            (state->errs2 >= 6 || (state->errs >= 2 && state->errs2 >= 5));
        if (many_fixed) state->imbe_gated++;
        if (opts->imbe_repeat &&
            ((state->errs >= 2 && (float)et >= 10.0f + 40.0f * state->imbe_error_rate) ||
             c0_sure_changed >= 1 || many_fixed)) {
            if (state->imbe_have_good && state->imbe_repeats_in_row < 3) {
                state->imbe_repeats_in_row++;
                state->imbe_repeated++;
                for (int b = 0; b < 11; b++) imbe88[b] = state->imbe_last_good[b];
                how = DSD_IMBE_REPEATED;
            } else {
                state->imbe_muted++;
                for (int k = 0; k < 160; k++) state->audio_out_temp_buf[k] = 0.0f;
                state->err_str[0] = 0;
                imbe_out(imbe88, DSD_IMBE_MUTED, state);
                goto audio;
            }
        } else {
            state->imbe_repeats_in_row = 0;
            for (int b = 0; b < 11; b++) state->imbe_last_good[b] = imbe88[b];
            state->imbe_have_good = 1;
        }
vocode:
    imbe_out(imbe88, how, state);
    if (!imbe_shim_try_decode_88(imbe88, snd)) return;
        for (int k = 0; k < 160; k++)
            state->audio_out_temp_buf[k] = (float)snd[k];
        state->err_str[0] = 0;
#else
        mbe_processImbe7200x4400Framef(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str, imbe_fr, imbe_d, state->cur_mp, state->prev_mp, state->prev_mp_enhanced, opts->uvquality);
#endif
    }
#if USE_OP25_VOCODER
audio:
#endif

    if (opts->errorbars == 1)
        printf("%s", state->err_str);

    state->debug_audio_errors += state->errs2;

    if (state->errs2 > p25_voice_gate) {
        for (int k = 0; k < 160; k++)
            state->audio_out_temp_buf[k] = 0.0f;
    }

    {
        extern void diag_line(const char *tag, const char *fmt, ...);
        static int vf_idx = 0;
        float pcm_max = 0.0f;
        for (int k = 0; k < 160; k++) {
            float a = state->audio_out_temp_buf[k];
            if (a < 0) a = -a;
            if (a > pcm_max) pcm_max = a;
        }
        diag_line("VFRM", "i=%d ones=%d/184 errs=%d errs2=%d repeat=%d pcm_max=%.0f str=%.10s",
                  vf_idx++, imbe_ones,
                  state->errs, state->errs2,
                  state->cur_mp->repeat, (double)pcm_max, state->err_str);
    }

    processAudio(opts, state);
}
