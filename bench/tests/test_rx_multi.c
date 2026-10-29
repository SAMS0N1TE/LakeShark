/* Several plugins on one RX HEAD stream, fed by one front from the LR2021's
   125 kS/s I/Q, the way FM runs POCSAG with the channel audible:
     - the voice delivers exactly the audio the LR chain alone does
       (test_fm_lr: 800 blocks make 32768 samples, the 1 kHz tone clean);
     - a pager beside it decodes the same page it decodes alone;
     - both read the one discriminator block per I/Q block. */
#include "ls_test.h"
#include "pocsag_gen.h"
#include "ls_rx_head.h"
#include "ls_rx_front.h"
#include "rx_pocsag.h"
#include "rx_voice.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

fm_state_t FM;

static fm_lr_dsp_t lr;
static fm_dsp_t dsp;
static ls_rx_front_t front;
static ls_rx_head_t H;

static int16_t s_pcm[40000];
static int s_n, s_writes;
static bool sink(void *u, const int16_t *pcm, int n, uint32_t rate)
{
    (void)u; (void)rate;
    for (int i = 0; i < n && s_n < (int)(sizeof(s_pcm) / sizeof(s_pcm[0])); i++) s_pcm[s_n++] = pcm[i];
    s_writes++;
    return false;
}

static void pack(uint8_t *p, double ph, double amp)
{
    const int16_t i = (int16_t)(amp * cos(ph)), q = (int16_t)(amp * sin(ph));
    p[0] = (uint8_t)i; p[1] = (uint8_t)((uint16_t)i >> 8);
    p[2] = (uint8_t)q; p[3] = (uint8_t)((uint16_t)q >> 8);
}

static void head(bool voice, bool pager)
{
    char why[64];
    const ls_rx_stream_t s = { .center_hz = 152600000, .rate_hz = 125000, .front = LS_RX_FRONT_NFM,
        .inputs = LS_RX_IQ_S16 | LS_RX_DISC | LS_RX_PCM16 };
    ls_rx_head_detach_all(&H);
    ls_rx_head_init(&H, &s);
    fm_lr_init(&lr, 25000);
    fm_dsp_init(&dsp);
    ls_rx_front_init(&front, &dsp, &lr);
    ls_rx_front_bind(&front, &H);
    ls_rx_head_set_audio(&H, sink, NULL, NULL);
    memset(&FM, 0, sizeof(FM));
    FM.pocsag_auto = true; FM.pocsag_baud = 1200;
    FM.squelch_tenths = 30;
    if (voice) {
        LS_CHECK(ls_rx_head_attach(&H, &rx_voice, why, sizeof(why)) >= 0);
        const rx_voice_cfg_t cfg = { .peak_cap = 6000, .pad_closed = true };
        rx_voice_bind(ls_rx_head_state(&H, &rx_voice), &FM, &cfg);
    }
    if (pager) {
        LS_CHECK(ls_rx_head_attach(&H, &rx_pocsag, why, sizeof(why)) >= 0);
        rx_pocsag_bind(ls_rx_head_state(&H, &rx_pocsag), &FM);
    }
    s_n = s_writes = 0;
}

/* test_fm_lr's signal: a 1 kHz tone at 3 kHz deviation, 800 blocks. */
static double tone_snr(int *total)
{
    double ph = 0; int k = 0;
    uint8_t iq[1280];
    for (int b = 0; b < 800; b++) {
        for (int n = 0; n < 320; n++, k++) {
            ph += 2 * PI * 3000 / 125000 * sin(2 * PI * 1000 * k / 125000.0);
            pack(iq + 4 * n, ph, 18000);
        }
        LS_CHECK(ls_rx_head_admit(&H, true));
        ls_rx_front_lr(&front, &H, iq, sizeof(iq), b);
    }
    double ss = 0, cc = 0, power = 0; int used = 0;
    for (int i = 8000; i < s_n && used < 16000; i++, used++) {
        const double a = 2 * PI * 1000 * used / 16000;
        ss += s_pcm[i] * sin(a); cc += s_pcm[i] * cos(a); power += (double)s_pcm[i] * s_pcm[i];
    }
    *total = s_n;
    const double signal = 2 * (ss * ss + cc * cc) / used;
    return 10 * log10(signal / (power - signal));
}

LS_CASE(voice_with_a_pager_beside_it_plays_what_it_plays_alone)
{
    int alone = 0, shared = 0;
    head(true, false);
    const double snr_alone = tone_snr(&alone);
    head(true, true);
    const double snr_shared = tone_snr(&shared);
    LS_CHECK(abs(alone - 32768) <= 1);
    LS_EQ_INT(shared, alone);
    LS_CHECK(snr_alone > 22);
    LS_NEAR(snr_shared, snr_alone, 0.01);
    LS_EQ_INT(s_writes, 800);
    LS_CHECK(FM.squelch_open);
    /* One discriminator block a block, for both. */
    LS_EQ_UINT(H.slot[0].blocks, 1600);   /* the voice: DISC and PCM16 */
    LS_EQ_UINT(H.slot[1].blocks, 800);    /* the pager: DISC */
    ls_rx_head_detach_all(&H);
}

/* A page sent over FM, heard with the voice on and with it off. */
static int page_through(bool voice)
{
    head(voice, true);
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);
    const size_t cap = pocsag_tx_page(&cfg, 1234560, 3, "ENGINE 7 CALL DISPATCH", NULL, 0);
    float *d = malloc(cap * sizeof(float));
    const size_t n = pocsag_tx_page(&cfg, 1234560, 3, "ENGINE 7 CALL DISPATCH", d, cap);
    /* 32 kHz discriminator to 125 kS/s phase, 4.5 kHz deviation. */
    const size_t total = n * 125000 / 32000;
    uint8_t iq[1280]; double ph = 0;
    for (size_t k = 0, b = 0; k < total; b++) {
        int m = 0;
        for (; m < 320 && k < total; m++, k++) {
            ph += 2 * PI * 4500 / 125000 * d[k * 32000 / 125000];
            pack(iq + 4 * m, ph, 18000);
        }
        ls_rx_front_lr(&front, &H, iq, (size_t)m * 4, (int64_t)b);
    }
    free(d);
    const int pages = FM.page_count;
    if (pages) {
        const fm_page_t *p = &FM.pages[(FM.page_head - 1 + FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX];
        LS_EQ_UINT(p->address, 1234560);
        LS_EQ_STR(p->text, "ENGINE 7 CALL DISPATCH");
    }
    ls_rx_head_detach_all(&H);
    return pages;
}

LS_CASE(the_pager_decodes_the_same_page_with_the_voice_on)
{
    LS_EQ_INT(page_through(false), 1);
    LS_EQ_INT(page_through(true), 1);
    LS_CHECK(s_writes > 0);   /* and the channel was audible meanwhile */
}

/* Behind: the pager is shed first and the voice keeps playing. */
LS_CASE(when_behind_the_pager_goes_before_the_voice)
{
    head(true, true);
    uint8_t iq[1280]; double ph = 0;
    for (int n = 0; n < 320; n++) { ph += 0.1; pack(iq + 4 * n, ph, 18000); }
    LS_CHECK(ls_rx_head_admit(&H, false));
    ls_rx_front_lr(&front, &H, iq, sizeof(iq), 0);
    LS_EQ_UINT(H.slot[0].blocks, 2);
    LS_EQ_UINT(H.slot[1].blocks, 0);
    LS_EQ_INT(s_writes, 1);
    LS_CHECK(!ls_rx_head_admit(&H, false));
    ls_rx_head_detach_all(&H);
}

/* POCSAG, FLEX and the voice on one stream: FM's pager preset with every
   paging decoder and the channel audible. The voice still plays exactly
   what it plays alone. */
#include "rx_fm_data.h"
LS_CASE(pocsag_flex_and_voice_together_keep_the_audio_whole)
{
    char why[64];
    int alone = 0, three = 0;
    head(true, false);
    tone_snr(&alone);
    head(true, true);
    LS_CHECK(ls_rx_head_attach(&H, &rx_flex, why, sizeof(why)) >= 0);
    LS_EQ_INT(H.n, 3);
    const double snr = tone_snr(&three);
    LS_EQ_INT(three, alone);
    LS_CHECK(snr > 22);
    LS_EQ_UINT(H.slot[1].blocks, 800);
    LS_EQ_UINT(H.slot[2].blocks, 800);
    ls_rx_head_detach_all(&H);
}

/* Every built-in plugin carries its contract: ABI, names, a doc a person can
   act on, the calls the head makes, and no internal RAM. */
LS_CASE(every_built_in_plugin_says_what_it_is)
{
    const ls_rx_plugin_t *all[] = { &rx_voice, &rx_pocsag, &rx_flex, &rx_aprs, &rx_same, &rx_ais, &rx_acars };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const ls_rx_plugin_t *p = all[i];
        LS_EQ_INT(p->abi, LS_RX_ABI);
        LS_CHECK(p->id && p->name && p->sub);
        LS_CHECK_MSG(p->doc && strlen(p->doc) > 40 && strstr(p->doc, "Test"), "%s: doc", p->id);
        LS_CHECK(p->attach && p->detach && p->reset && p->process);
        LS_EQ_UINT(p->needs.internal_bytes, 0);
        LS_CHECK(p->needs.inputs != 0);
    }
}
