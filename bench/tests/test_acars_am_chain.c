/* ACARS over the air is 2400 baud MSK audio on an AM carrier. This puts a
   generated message on a carrier as the RTL-SDR (8-bit I/Q, 256 kS/s) and
   the LR2021 (16-bit I/Q, 125 kS/s) deliver it, and runs each receive chain
   into the decoder: the AM envelope decodes it, the FM discriminator does
   not (that was the bug). */
#include "ls_test.h"
#include "acars_gen.h"
#include "acars.h"
#include "acars_resample.h"
#include "fm_dsp.h"
#include "fm_lr_dsp.h"
#include "fm_state.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_CAP (ACARS_SAMP_RATE * 2)

static const acars_msg_t MSG = {
    .mode = '2', .reg = ".N123AB", .tak = 0x15, .label = "H1", .block_id = '1',
    .text = "LAKESHARK AM CHAIN TEST",
};

/* The message as decoder-rate audio, with a second of silence either side. */
static float *message_audio(size_t *n)
{
    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);
    float *a = calloc(AUDIO_CAP, sizeof(float));
    size_t lead = ACARS_SAMP_RATE / 4;
    size_t len = acars_tx_msg(&cfg, &MSG, a + lead, AUDIO_CAP - 2 * lead);
    *n = len + 2 * lead;
    return a;
}

/* Audio sample at time t (seconds), linear between decoder-rate samples. */
static float audio_at(const float *a, size_t n, double t)
{
    double x = t * ACARS_SAMP_RATE;
    size_t i = (size_t)x;
    if (i + 1 >= n) return 0.0f;
    double f = x - (double)i;
    return (float)(a[i] * (1.0 - f) + a[i + 1] * f);
}

typedef struct { acars_state_t state; acars_ctx_t *c; acars_rs_t rs; } rx_t;

static void rx_init(rx_t *r)
{
    memset(r, 0, sizeof(*r));
    r->c = acars_create(&r->state);
    acars_rs_init(&r->rs);
}

static void rx_feed(rx_t *r, const float *d32k, int n)
{
    float out[2048];
    int na = acars_rs_5to3(&r->rs, d32k, n, out, 2048);
    if (na > 0) acars_process(r->c, out, na);
}

/* Modulation depth, carrier offset from tune (Hz) and level in 8-bit counts. */
static double DEPTH = 0.6, OFFSET = 1500.0, AMP = 50.0;
/* RTL: 8-bit offset I/Q at FM_RTL_RATE. */
static int rtl_chain(bool envelope)
{
    size_t na; float *a = message_audio(&na);
    const double secs = (double)na / ACARS_SAMP_RATE;
    const size_t pairs = (size_t)(secs * FM_RTL_RATE);
    uint8_t *iq = malloc(pairs * 2);
    for (size_t k = 0; k < pairs; k++) {
        double t = (double)k / FM_RTL_RATE;
        double env = 1.0 + DEPTH * audio_at(a, na, t);
        double ph = 2.0 * M_PI * OFFSET * t;
        iq[2 * k]     = (uint8_t)lround(127.0 + AMP * env * cos(ph));
        iq[2 * k + 1] = (uint8_t)lround(127.0 + AMP * env * sin(ph));
    }
    static fm_dsp_t dsp;
    fm_dsp_init(&dsp);
    rx_t r; rx_init(&r);
    float d[1100];
    for (size_t off = 0; off + 2 <= pairs * 2; off += 16384) {
        int len = (int)((pairs * 2 - off) < 16384 ? (pairs * 2 - off) : 16384);
        int nd = envelope ? fm_demod_env(&dsp, iq + off, len, d, 1100)
                          : fm_demod_iq(&dsp, iq + off, len, d, 1100);
        rx_feed(&r, d, nd);
    }
    int msgs = (int)r.state.msg_count;
    acars_destroy(r.c); free(iq); free(a);
    return msgs;
}

/* LR2021: signed 16-bit I/Q at 125 kS/s, carrier 1.5 kHz off tune. */
static int lr_chain(bool envelope)
{
    size_t na; float *a = message_audio(&na);
    const double secs = (double)na / ACARS_SAMP_RATE;
    const size_t pairs = (size_t)(secs * 125000.0);
    uint8_t *iq = malloc(pairs * 4);
    for (size_t k = 0; k < pairs; k++) {
        double t = (double)k / 125000.0;
        double env = 1.0 + 0.6 * audio_at(a, na, t);
        double ph = 2.0 * M_PI * 1500.0 * t;
        int16_t i = (int16_t)lround(4000.0 * env * cos(ph));
        int16_t q = (int16_t)lround(4000.0 * env * sin(ph));
        iq[4 * k] = (uint8_t)i; iq[4 * k + 1] = (uint8_t)((uint16_t)i >> 8);
        iq[4 * k + 2] = (uint8_t)q; iq[4 * k + 3] = (uint8_t)((uint16_t)q >> 8);
    }
    static fm_lr_dsp_t lr; static fm_dsp_t dsp;
    fm_lr_init(&lr, 25000); fm_dsp_init(&dsp);
    rx_t r; rx_init(&r);
    float d[100];
    for (size_t off = 0; off + 4 <= pairs * 4; off += 1280) {
        size_t len = pairs * 4 - off < 1280 ? pairs * 4 - off : 1280;
        int nd = envelope ? fm_lr_demod_env(&lr, &dsp, iq + off, len, d, 100)
                          : fm_lr_demod(&lr, &dsp, iq + off, len, d, 100);
        rx_feed(&r, d, nd);
    }
    int msgs = (int)r.state.msg_count;
    acars_destroy(r.c); free(iq); free(a);
    return msgs;
}

LS_CASE(rtl_am_envelope_decodes_acars_and_the_discriminator_does_not)
{
    LS_EQ_INT(rtl_chain(true), 1);
    LS_EQ_INT(rtl_chain(false), 0);
}

LS_CASE(rtl_envelope_holds_across_depth_offset_and_level)
{
    /* The binomial decimator this replaced decoded 3 of these 18. */
    const double depths[] = {0.3, 0.6, 0.9}, offs[] = {0, 1500, 4000}, amps[] = {20, 50};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) for (int k = 0; k < 2; k++) {
        DEPTH = depths[i]; OFFSET = offs[j]; AMP = amps[k];
        LS_CHECK_MSG(rtl_chain(true) == 1, "envelope chain missed a message");
    }
    DEPTH = 0.6; OFFSET = 1500.0; AMP = 50.0;
}

LS_CASE(lr2021_am_envelope_decodes_acars_and_the_discriminator_does_not)
{
    LS_EQ_INT(lr_chain(true), 1);
    LS_EQ_INT(lr_chain(false), 0);
}
