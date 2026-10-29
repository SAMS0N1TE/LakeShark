/* See rx_voice.h. Squelch and tone are fm_dsp.c and fm_tone.c. */
#include "rx_voice.h"
#include "ls_rx_head.h"
#include "ls_rx_front.h"
#include "fm_tone.h"
#include "esp_heap_caps.h"
#include <string.h>

typedef struct {
    ls_rx_head_t *h;
    fm_state_t   *out;
    rx_voice_cfg_t cfg;
    bool          gate;   /* the squelch for this block's audio */
    fm_tone_t     tone;
    int16_t       pcm[LS_RX_FRONT_PCM_MAX];
} rx_voice_t;

static void *attach(ls_rx_head_t *h, const ls_rx_stream_t *s, char *why, size_t n)
{
    (void)s; (void)why; (void)n;
    rx_voice_t *v = heap_caps_calloc(1, sizeof(*v), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!v) return NULL;
    v->h = h;
    fm_tone_init(&v->tone);
    return v;
}

static void detach(void *st) { heap_caps_free(st); }

void rx_voice_bind(void *st, fm_state_t *out, const rx_voice_cfg_t *cfg)
{
    rx_voice_t *v = st;
    if (!v) return;
    v->out = out;
    if (cfg) v->cfg = *cfg;
    if (out) out->tone_detected = v->tone.result;
}

static void reset(void *st, ls_rx_reset_t why)
{
    (void)why;
    rx_voice_t *v = st;
    fm_tone_init(&v->tone);
    v->gate = false;
    if (v->out) { v->out->squelch_open = false; v->out->tone_detected = v->tone.result; }
}

static void squelch(rx_voice_t *v, const ls_rx_block_t *b)
{
    fm_dsp_t *dsp = ls_rx_front_dsp(v->h);
    const int sq = v->out ? v->out->squelch_tenths : 0;
    const uint16_t tone = v->out ? v->out->tone_required : 0;
    int gate = sq <= 0 ? 1 : sq >= 100 ? 0 : dsp ? fm_nfm_squelch(dsp, sq, (int)b->n) : 1;
    gate = fm_tone_receive(&v->tone, b->data, (int)b->n, gate, tone);
    v->gate = gate;
    if (!v->out) return;
    v->out->tone_detected = v->tone.result;
    if (tone && gate) v->out->tone_detected.selection = tone;
    if (dsp) v->out->noise = dsp->demod_noise;
    v->out->squelch_open = gate;
    ls_rx_audio_gate(v->h, gate);
}

static void audio(rx_voice_t *v, const ls_rx_block_t *b)
{
    if (!v->gate && !v->cfg.pad_closed) return;
    int n = (int)b->n;
    if (n > LS_RX_FRONT_PCM_MAX) n = LS_RX_FRONT_PCM_MAX;
    /* A copy: decoders after this one read the front's audio untouched. */
    const int16_t *in = b->data;
    const int16_t cap = v->cfg.peak_cap;
    for (int i = 0; i < n; i++) {
        int16_t s = v->gate ? in[i] : 0;
        if (cap && s > cap) s = cap;
        else if (cap && s < -cap) s = (int16_t)-cap;
        v->pcm[i] = s;
    }
    ls_rx_audio(v->h, v->pcm, n, b->rate_hz);
}

static void process(void *st, const ls_rx_block_t *b)
{
    if (b->input == LS_RX_DISC) squelch(st, b);
    else if (b->input == LS_RX_PCM16) audio(st, b);
}

static void status(const void *st, ls_rx_status_t *out)
{
    const rx_voice_t *v = st;
    out->synced = v->gate;
    out->detail = v->tone.result.selection;
}

const ls_rx_plugin_t rx_voice = {
    .abi = LS_RX_ABI, .id = "voice", .name = "NFM",
    .sub = "narrowband FM voice, squelch and tone", .maturity = LS_RX_WORKS,
    .needs = {
        .inputs = LS_RX_DISC | LS_RX_PCM16, .front = LS_RX_FRONT_NFM,
        .channel_bw_hz = 12500, .min_rate_hz = 32000, .rsel_job = -1,
        .psram_bytes = sizeof(rx_voice_t), .cpu_permille = 40, .flags = LS_RX_F_AUDIO,
    },
    .doc = "NFM voice: noise squelch, CTCSS/DCS gate, audio and CALLS. Test: MODE NFM, a repeater.",
    .attach = attach, .detach = detach, .reset = reset, .process = process, .status = status,
};
