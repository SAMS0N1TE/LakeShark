#include "dsd.h"
#include "esp_log.h"

static const char *TAG_DSD = "DSD_INIT";

int exitflag = 0;

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
    opts->errorbars = 1;
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
    opts->msize = 15;
    opts->use_cosine_filter = 1;
    opts->unmute_encrypted_p25 = 0;
    opts->audio_gain = 0.0f;
    opts->audio_out = 1;
    opts->symboltiming = 0;
    opts->datascope = 0;
    opts->scoperate = 15;
    opts->ring = NULL;
}

void initState(dsd_state *state)
{
    int i;
    state->dibit_buf = (int *)malloc(sizeof(int) * 10000);
    ESP_LOGI(TAG_DSD, "dibit_buf: %p (%d bytes)", state->dibit_buf, (int)(sizeof(int) * 10000));
    if (!state->dibit_buf) { ESP_LOGE(TAG_DSD, "dibit_buf alloc FAILED"); return; }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->repeat = 0;
    state->audio_out_buf = (short *)malloc(sizeof(short) * 2000);
    ESP_LOGI(TAG_DSD, "audio_out_buf: %p", state->audio_out_buf);
    if (!state->audio_out_buf) { ESP_LOGE(TAG_DSD, "audio_out_buf alloc FAILED"); return; }
    state->audio_out_buf_p = state->audio_out_buf;
    state->audio_out_float_buf = (float *)malloc(sizeof(float) * 2000);
    ESP_LOGI(TAG_DSD, "audio_out_float_buf: %p", state->audio_out_float_buf);
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
    /* Session 9: DSP pipeline now delivers 24 kHz audio → 5 samples per
     * P25 symbol (not 10). symbolCenter=2 means the middle sample is at
     * index 2 of the 5-sample window. getSymbol now does median-of-5 for
     * C4FM (rf_mod==0) to reject impulse noise / phase-wrap spikes. */
    state->samplesPerSymbol = 5;
    state->symbolCenter = 2;
    /* Session 10: C4FM clock-assist TED. Default M&M (mode=2) — uses sliced
     * decisions, data-aided, most robust. Set to 0 to disable. Set to 1 for
     * Early-Late energy-difference mode (non-data-aided fallback). */
    state->c4fm_clk_mode = 2;
    state->c4fm_clk_prev_dec = 0;
    state->c4fm_clk_run_dir = 0;
    state->c4fm_clk_run_len = 0;
    state->c4fm_clk_cooldown = 0;
    state->c4fm_clk_nudges = 0;
    memset(state->algid, 0, 9);
    memset(state->keyid, 0, 17);
    state->currentslot = 0;
    state->cur_mp = (mbe_parms *)malloc(sizeof(mbe_parms));
    state->prev_mp = (mbe_parms *)malloc(sizeof(mbe_parms));
    state->prev_mp_enhanced = (mbe_parms *)malloc(sizeof(mbe_parms));
    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    state->p25kid = 0;
    state->debug_audio_errors = 0;
    state->debug_header_errors = 0;
    state->debug_header_critical_errors = 0;
    state->last_dibit = 0;
    initialize_p25_heuristics(&state->p25_heuristics);
    initialize_p25_heuristics(&state->inv_p25_heuristics);
    state->pcm_out_buf = NULL;
    state->pcm_out_write = 0;
    state->pcm_out_size = 0;
}

void noCarrier(dsd_opts *opts, dsd_state *state)
{
    state->dibit_buf_p = state->dibit_buf + 200;
    memset(state->dibit_buf, 0, sizeof(int) * 200);
    state->jitter = -1;
    state->lastsynctype = -1;
    state->carrier = 0;
    state->max = 15000;
    state->min = -15000;
    state->center = 0;
    /* Reseed the rolling avg ring; otherwise stale low values from the
     * previous acquisition drag state->max down on the next one. */
    for (int i = 0; i < (int)(sizeof(state->minbuf)/sizeof(state->minbuf[0])); i++) {
        state->minbuf[i] = -15000;
        state->maxbuf[i] =  15000;
    }
    state->midx = 0;
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
    /* Session 10: reset short-term TED detector state so a new carrier starts
     * with a clean error-accumulation history. Deliberately PRESERVE
     * symbolCenter (the learned timing offset) and c4fm_clk_mode (user pref)
     * — otherwise every brief signal loss would throw away sample-clock
     * alignment gained over hundreds of symbols. */
    state->c4fm_clk_prev_dec = 0;
    state->c4fm_clk_run_dir = 0;
    state->c4fm_clk_run_len = 0;
    state->c4fm_clk_cooldown = 0;
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

    /* Use a sensible target headroom (0.7 × full-scale) instead of
     * pegging at 32767, so transients don't clip. */
    if (amax > 0.0f)
        gainfactor = (22937.0f / amax);   /* 0.7 * 32767 */
    else
        gainfactor = 50.0f;

    /* Clamp to reasonable bounds so mbelib's occasional near-zero
     * output doesn't make us jack the gain to absurd values that
     * instantly blow up real audio when it returns. */
    if (gainfactor > 200.0f) gainfactor = 200.0f;
    if (gainfactor < 1.0f)   gainfactor = 1.0f;

    /* One-sided attack smoothing: slow ramp UP to prevent pop,
     * instant ramp DOWN so loud onsets don't clip. */
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

        /* Historical DSD path writes into audio_out_buf as well. Our
         * allocation is only 2000 shorts, and this pointer advances
         * 160 samples per IMBE (1440/LDU, unbounded across LDUs),
         * so without a wrap we trash heap after ~12 IMBEs.
         * The legacy output is unused on ESP32, but the write-through
         * used to corrupt adjacent allocations, so we just gate it. */
        if (state->audio_out_buf_p &&
            state->audio_out_idx < 1900) {
            state->audio_out_buf_p[0] = (short)sample;
            state->audio_out_buf_p++;
            state->audio_out_idx++;
        } else if (state->audio_out_buf) {
            /* Wrap back to start once we fill the buffer. */
            state->audio_out_buf_p = state->audio_out_buf;
            state->audio_out_idx = 0;
        }
    }
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
        mbe_processImbe7200x4400Framef(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str, imbe_fr, imbe_d, state->cur_mp, state->prev_mp, state->prev_mp_enhanced, opts->uvquality);
    }

    if (opts->errorbars == 1)
        printf("%s", state->err_str);

    state->debug_audio_errors += state->errs2;

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
