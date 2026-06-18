#include "dsd.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

/* IMBE voice synthesis backend: 1 = OP25 fixed-point imbe_vocoder (fast, no
 * cosf), 0 = float mbelib (kept as fallback). Both share mbelib's FEC. */
#define USE_OP25_VOCODER 1
#include "imbe_shim.h"

volatile int p25_voice_gate = 99;

/* DSD working buffers are CPU-only (no DMA) and several KB each. On the
 * memory-tight mesh-gateway build internal RAM is exhausted by the time the
 * P25 decoder task runs, so the big/cold ones go in PSRAM. */
#define DSD_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM)

/* HOT buffers the IMBE synthesizer touches every frame (the mbe_parms and the
 * audio output buffers). In PSRAM each LDU's synthesis costs 50-200 ms -- and on
 * the LCD build it contends with the DSI framebuffer DMA -- which is what made
 * decoded P25 voice choppy. Prefer internal SRAM; fall back to PSRAM if internal
 * is exhausted, so this never makes allocation fail (just slower, as before). */
static inline void *dsd_malloc_fast(size_t sz)
{
    /* Only spend internal SRAM on the hot buffers when there's comfortable
     * headroom AFTER the alloc. On the LCD build internal RAM is nearly
     * exhausted by the time P25 starts (~8 KB free), and grabbing it for these
     * buffers left only ~900 bytes for the rest of the system -- one stray
     * internal alloc from crashing. Keep a 24 KB internal reserve: the tight
     * LCD board falls back to PSRAM (safe, a bit slower), while the roomy
     * headless Nano build still gets the fast internal path. */
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > sz + 24576) {
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
    opts->errorbars = 0;   /* was 1: printf per IMBE voice frame blocked the
                            * UART (~450 writes/s) and stalled the decoder */
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
    opts->msize = 256;   /* see app_p25.c — widened from 15 to steady slicer thresholds */
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
    state->dibit_buf = (int *)DSD_MALLOC(sizeof(int) * 10000);
    ESP_LOGI(TAG_DSD, "dibit_buf: %p (%d bytes)", state->dibit_buf, (int)(sizeof(int) * 10000));
    if (!state->dibit_buf) { ESP_LOGE(TAG_DSD, "dibit_buf alloc FAILED"); return; }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->repeat = 0;
    state->audio_out_buf = (short *)DSD_MALLOC_FAST(sizeof(short) * 2000);
    ESP_LOGI(TAG_DSD, "audio_out_buf: %p", state->audio_out_buf);
    if (!state->audio_out_buf) { ESP_LOGE(TAG_DSD, "audio_out_buf alloc FAILED"); return; }
    state->audio_out_buf_p = state->audio_out_buf;
    state->audio_out_float_buf = (float *)DSD_MALLOC_FAST(sizeof(float) * 2000);
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
    /* DSP pipeline delivers 48 kHz audio → 10 samples per P25 symbol, matching
     * the dsd-fme reference (reverted from session-9's 24kHz/5sps). symbolCenter=4
     * = the middle of the 10-sample window. At 10 sps getSymbol uses the
     * averaging path (sum symbolCenter-1..+2), not the 5-sps median; this gives
     * 2x symbol-timing resolution so the ±1-sample clock nudge can actually
     * center on the plateau (5 sps left sync stuck at Hamming-distance 4). */
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
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
    state->cur_mp = (mbe_parms *)DSD_MALLOC_FAST(sizeof(mbe_parms));
    state->prev_mp = (mbe_parms *)DSD_MALLOC_FAST(sizeof(mbe_parms));
    state->prev_mp_enhanced = (mbe_parms *)DSD_MALLOC_FAST(sizeof(mbe_parms));
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
    /* PRESERVE the symbol-amplitude envelope (min/max/center + the
     * minbuf/maxbuf rolling-average ring + midx) across a sync loss, matching
     * the upstream dsd-fme reference (its noCarrier never touches these).
     *
     * The previous code hard-reset min/max to ±15000 and reseeded the whole
     * averaging ring to ±15000 on every sync drop. But the real C4FM outer
     * rail is only ~±5000-8000, so each reset forced the slicer thresholds
     * (umid/lmid) 2-3x too high for the next ~msize symbols — +3 outer symbols
     * then sliced as +1 inner, corrupting dibits, failing BCH, dropping sync,
     * and calling noCarrier again: a feedback loop that made bursty/conventional
     * signals decode choppily and never let BCH pass (observed live: nac
     * flickering D4D/DB8, bchFAIL only, umid/lmid swinging -8756..+5118).
     *
     * use_symbol() retracks min/max from the live signal every symbol, so a
     * preserved (slightly stale) envelope self-corrects within a few symbols
     * instead of starting from a wildly-wrong ±15000 each time. */
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
#if USE_OP25_VOCODER
        /* Keep mbelib's (cheap, correct) FEC + deinterleave to fill imbe_d[88]
         * = u0..u7 (12,12,12,12,11,11,11,7) MSB-first, then hand it to OP25's
         * fixed-point synthesizer (no cosf hot loop). imbe_d's bit order matches
         * decode_4400's input exactly, so no remap is needed. */
        state->errs  = mbe_eccImbe7200x4400C0(imbe_fr);
        mbe_demodulateImbe7200x4400Data(imbe_fr);
        state->errs2 = mbe_eccImbe7200x4400Data(imbe_fr, imbe_d);
        state->debug_audio_errors += 0;
        uint8_t imbe88[11];
        for (int b = 0; b < 11; b++) imbe88[b] = 0;
        for (int b = 0; b < 88; b++)
            if (imbe_d[b] & 1) imbe88[b >> 3] |= (uint8_t)(0x80 >> (b & 7));
        int16_t snd[160];
        imbe_shim_decode_88(imbe88, snd);
        for (int k = 0; k < 160; k++)
            state->audio_out_temp_buf[k] = (float)snd[k];
        state->err_str[0] = 0;
#else
        mbe_processImbe7200x4400Framef(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str, imbe_fr, imbe_d, state->cur_mp, state->prev_mp, state->prev_mp_enhanced, opts->uvquality);
#endif
    }

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
