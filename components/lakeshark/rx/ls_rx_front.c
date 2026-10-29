/* See ls_rx_front.h. */
#include "ls_rx_front.h"
#include <math.h>
#include <string.h>

void ls_rx_front_init(ls_rx_front_t *f, fm_dsp_t *dsp, fm_lr_dsp_t *lr)
{
    f->dsp = dsp; f->lr = lr;
    f->nd = f->na = 0; f->rms = 0.0f;
}

void ls_rx_front_bind(ls_rx_front_t *f, ls_rx_head_t *h) { h->front = f; }

fm_dsp_t *ls_rx_front_dsp(const ls_rx_head_t *h)
{
    return h && h->front ? ((const ls_rx_front_t *)h->front)->dsp : NULL;
}

static float rms_f(const float *x, int n)
{
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    return n > 0 ? sqrtf(ss / (float)n) : 0.0f;
}

static void feed(ls_rx_head_t *h, uint32_t input, const void *data, int n, uint32_t rate, int64_t at)
{
    if (n <= 0) return;
    const ls_rx_block_t b = { .input = input, .data = data, .n = (uint32_t)n, .rate_hz = rate, .at_us = at };
    ls_rx_head_feed(h, &b);
}

/* After the discriminator: DISC, then audio from it when wanted. */
static void nfm_out(ls_rx_front_t *f, ls_rx_head_t *h, uint32_t wants, int pcm_cap, int64_t at)
{
    f->rms = rms_f(f->disc, f->nd);
    if (wants & LS_RX_DISC) feed(h, LS_RX_DISC, f->disc, f->nd, FM_DEMOD_RATE, at);
    if (wants & LS_RX_PCM16) {
        f->na = fm_demod_to_audio(f->dsp, f->disc, f->nd, f->pcm, pcm_cap);
        feed(h, LS_RX_PCM16, f->pcm, f->na, FM_AUDIO_RATE, at);
    }
}

int ls_rx_front_lr(ls_rx_front_t *f, ls_rx_head_t *h, const uint8_t *iq, size_t bytes, int64_t at_us)
{
    const uint32_t wants = ls_rx_head_wants(h);
    f->nd = f->na = 0;
    if (wants & LS_RX_IQ_S16) feed(h, LS_RX_IQ_S16, iq, (int)(bytes / 4), 125000, at_us);
    switch (h->stream.front) {
    case LS_RX_FRONT_NFM:
        if (!(wants & (LS_RX_DISC | LS_RX_PCM16))) break;
        f->nd = fm_lr_demod(f->lr, f->dsp, iq, bytes, f->disc, LS_RX_FRONT_LR_DISC);
        nfm_out(f, h, wants, LS_RX_FRONT_LR_PCM, at_us);
        break;
    case LS_RX_FRONT_AM:
        if (wants & LS_RX_ENV) {
            f->nd = fm_lr_demod_env(f->lr, f->dsp, iq, bytes, f->disc, LS_RX_FRONT_LR_DISC);
            f->rms = rms_f(f->disc, f->nd);
            feed(h, LS_RX_ENV, f->disc, f->nd, FM_DEMOD_RATE, at_us);
        }
        break;
    default:
        break;
    }
    return f->nd;
}

int ls_rx_front_u8(ls_rx_front_t *f, ls_rx_head_t *h, const uint8_t *iq, int len, int64_t at_us)
{
    const uint32_t wants = ls_rx_head_wants(h);
    f->nd = f->na = 0;
    if (wants & LS_RX_IQ_U8) feed(h, LS_RX_IQ_U8, iq, len / 2, FM_RTL_RATE, at_us);
    switch (h->stream.front) {
    case LS_RX_FRONT_NFM:
        if (!(wants & (LS_RX_DISC | LS_RX_PCM16))) break;
        f->nd = fm_demod_iq(f->dsp, iq, len, f->disc, LS_RX_FRONT_DISC_MAX);
        nfm_out(f, h, wants, LS_RX_FRONT_PCM_MAX, at_us);
        break;
    case LS_RX_FRONT_AM:
        if (wants & LS_RX_ENV) {
            f->nd = fm_demod_env(f->dsp, iq, len, f->disc, LS_RX_FRONT_DISC_MAX);
            f->rms = rms_f(f->disc, f->nd);
            feed(h, LS_RX_ENV, f->disc, f->nd, FM_DEMOD_RATE, at_us);
        }
        break;
    default:
        break;
    }
    return f->nd;
}
