/* RX HEAD core on the host: admission, dispatch, shedding, audio and tune
   routing, and the front computing each input once for every plugin. */
#include "ls_test.h"
#include "ls_rx_head.h"
#include "ls_rx_front.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

fm_state_t FM;

typedef struct {
    int attached, detached, resets[4], blocks[16], retunes;
    uint32_t last_n[16];
    const void *last_data[16];
} fake_t;

static fake_t s_fake[6];
static int s_next;

static int bit_index(uint32_t b) { int i = 0; while (b > 1) { b >>= 1; i++; } return i; }

static void *f_attach(ls_rx_head_t *h, const ls_rx_stream_t *s, char *why, size_t n)
{
    (void)h; (void)s; (void)why; (void)n;
    fake_t *f = &s_fake[s_next++ % 6];
    memset(f, 0, sizeof(*f));
    f->attached = 1;
    return f;
}
static void f_detach(void *st) { ((fake_t *)st)->detached++; }
static void f_reset(void *st, ls_rx_reset_t why) { ((fake_t *)st)->resets[why]++; }
static void f_retune(void *st, uint32_t hz) { (void)hz; ((fake_t *)st)->retunes++; }
static void f_process(void *st, const ls_rx_block_t *b)
{
    fake_t *f = st;
    const int i = bit_index(b->input);
    f->blocks[i]++; f->last_n[i] = b->n; f->last_data[i] = b->data;
}
static void *f_attach_fail(ls_rx_head_t *h, const ls_rx_stream_t *s, char *why, size_t n)
{
    (void)h; (void)s;
    snprintf(why, n, "no PSRAM");
    return NULL;
}

#define FAKE(nm, in, fr, fl, cpu) { .abi = LS_RX_ABI, .id = nm, .name = nm, .doc = "test", \
    .needs = { .inputs = (in), .front = (fr), .flags = (fl), .cpu_permille = (cpu), .rsel_job = -1 }, \
    .attach = f_attach, .detach = f_detach, .reset = f_reset, .retune = f_retune, .process = f_process }

static const ls_rx_plugin_t VOICE  = FAKE("VOICE", LS_RX_DISC | LS_RX_PCM16, LS_RX_FRONT_NFM, LS_RX_F_AUDIO, 100);
static const ls_rx_plugin_t VOICE2 = FAKE("VOICE2", LS_RX_PCM16, LS_RX_FRONT_NFM, LS_RX_F_AUDIO, 100);
static const ls_rx_plugin_t PAGER  = FAKE("PAGER", LS_RX_DISC | LS_RX_BITS, LS_RX_FRONT_NFM, 0, 100);
static const ls_rx_plugin_t DATA16 = FAKE("DATA16", LS_RX_PCM16, LS_RX_FRONT_NFM, 0, 100);
static const ls_rx_plugin_t TUNER  = FAKE("TUNER", LS_RX_DISC, LS_RX_FRONT_NFM, LS_RX_F_TUNES, 50);
static const ls_rx_plugin_t TUNER2 = FAKE("TUNER2", LS_RX_DISC, LS_RX_FRONT_NFM, LS_RX_F_TUNES, 50);
static const ls_rx_plugin_t ALONE  = FAKE("ALONE", LS_RX_DISC, LS_RX_FRONT_NFM, LS_RX_F_EXCLUSIVE, 50);
static const ls_rx_plugin_t HOG    = FAKE("HOG", LS_RX_DISC, LS_RX_FRONT_NFM, 0, 800);
static const ls_rx_plugin_t ACARS  = FAKE("ACARS", LS_RX_ENV, LS_RX_FRONT_AM, 0, 100);
static const ls_rx_plugin_t PKT    = FAKE("PKT", LS_RX_PACKET, LS_RX_FRONT_NONE, 0, 10);
static const ls_rx_plugin_t OLDABI = { .abi = 0, .id = "old", .name = "OLD", .doc = "x",
    .needs = { .inputs = LS_RX_DISC }, .process = f_process };
static const ls_rx_plugin_t NODOC  = { .abi = LS_RX_ABI, .id = "nodoc", .name = "NODOC",
    .needs = { .inputs = LS_RX_DISC }, .process = f_process };
static const ls_rx_plugin_t BROKE  = { .abi = LS_RX_ABI, .id = "broke", .name = "BROKE", .doc = "x",
    .needs = { .inputs = LS_RX_DISC, .front = LS_RX_FRONT_NFM }, .attach = f_attach_fail, .process = f_process };

static ls_rx_head_t H;

static void nfm_head(void)
{
    const ls_rx_stream_t s = { .center_hz = 152600000, .rate_hz = 125000, .front = LS_RX_FRONT_NFM,
        .inputs = LS_RX_IQ_S16 | LS_RX_DISC | LS_RX_PCM16 };
    ls_rx_head_init(&H, &s);
    s_next = 0;
}

LS_CASE(attach_admits_only_what_the_stream_and_the_others_allow)
{
    char why[64];
    nfm_head();
    LS_EQ_INT(ls_rx_head_attach(&H, &OLDABI, why, sizeof(why)), -1);
    LS_EQ_INT(ls_rx_head_attach(&H, &NODOC, why, sizeof(why)), -1);
    LS_EQ_INT(ls_rx_head_attach(&H, &ACARS, why, sizeof(why)), -1);   /* AM front on an NFM stream */
    LS_CHECK(strstr(why, "ACARS") != NULL);
    LS_EQ_INT(ls_rx_head_attach(&H, &PKT, why, sizeof(why)), -1);     /* no packets here */
    LS_EQ_INT(ls_rx_head_attach(&H, &BROKE, why, sizeof(why)), -1);
    LS_CHECK(strstr(why, "PSRAM") != NULL);

    LS_EQ_INT(ls_rx_head_attach(&H, &PAGER, why, sizeof(why)), 0);
    LS_EQ_INT(ls_rx_head_attach(&H, &PAGER, why, sizeof(why)), 0);   /* attaching twice is one */
    /* The voice goes in front of the data plugins. */
    LS_EQ_INT(ls_rx_head_attach(&H, &VOICE, why, sizeof(why)), 0);
    LS_CHECK(H.slot[1].p == &PAGER);
    LS_CHECK(ls_rx_head_has_audio(&H));
    LS_EQ_INT(ls_rx_head_attach(&H, &VOICE2, why, sizeof(why)), -1);
    LS_CHECK(strstr(why, "audio") != NULL);
    LS_CHECK(ls_rx_head_attach(&H, &TUNER, why, sizeof(why)) >= 0);
    LS_EQ_INT(ls_rx_head_attach(&H, &TUNER2, why, sizeof(why)), -1);
    LS_EQ_INT(ls_rx_head_attach(&H, &ALONE, why, sizeof(why)), -1);
    LS_EQ_INT(ls_rx_head_attach(&H, &HOG, why, sizeof(why)), -1);    /* 250 + 800 > 900 */
    LS_CHECK(strstr(why, "CPU") != NULL);
    LS_EQ_INT(H.n, 3);

    char chips[64];
    ls_rx_head_chips(&H, chips, sizeof(chips));
    LS_EQ_STR(chips, "VOICE PAGER TUNER");

    fake_t *pager = ls_rx_head_state(&H, &PAGER);
    ls_rx_head_detach(&H, &PAGER);
    LS_EQ_INT(pager->detached, 1);
    LS_EQ_INT(H.n, 2);
    LS_CHECK(ls_rx_head_state(&H, &PAGER) == NULL);
    ls_rx_head_detach_all(&H);
    LS_EQ_INT(H.n, 0);

    /* An exclusive plugin keeps the stream to itself. */
    LS_EQ_INT(ls_rx_head_attach(&H, &ALONE, why, sizeof(why)), 0);
    LS_EQ_INT(ls_rx_head_attach(&H, &PAGER, why, sizeof(why)), -1);
    ls_rx_head_detach_all(&H);
}

LS_CASE(the_raw_inputs_of_a_plugin_work_whatever_the_front)
{
    char why[64];
    const ls_rx_stream_t s = { .front = LS_RX_FRONT_NONE, .inputs = LS_RX_BITS | LS_RX_BATCH };
    ls_rx_head_init(&H, &s);
    LS_EQ_INT(ls_rx_head_attach(&H, &PAGER, why, sizeof(why)), 0);
    LS_EQ_UINT(ls_rx_head_wants(&H), LS_RX_BITS);
    uint8_t bytes[8] = {0};
    const ls_rx_block_t b = { .input = LS_RX_BITS, .data = bytes, .n = 8, .rate_hz = 1200 };
    ls_rx_head_feed(&H, &b);
    fake_t *f = ls_rx_head_state(&H, &PAGER);
    LS_EQ_INT(f->blocks[bit_index(LS_RX_BITS)], 1);
    ls_rx_head_detach_all(&H);
}

LS_CASE(blocks_reach_only_the_plugins_that_take_them)
{
    char why[64];
    nfm_head();
    ls_rx_head_attach(&H, &PAGER, why, sizeof(why));
    ls_rx_head_attach(&H, &DATA16, why, sizeof(why));
    LS_EQ_UINT(ls_rx_head_wants(&H), LS_RX_DISC | LS_RX_PCM16);
    float d[4] = {0};
    const ls_rx_block_t b = { .input = LS_RX_DISC, .data = d, .n = 4, .rate_hz = 32000 };
    ls_rx_head_feed(&H, &b);
    fake_t *p = ls_rx_head_state(&H, &PAGER), *q = ls_rx_head_state(&H, &DATA16);
    LS_EQ_INT(p->blocks[bit_index(LS_RX_DISC)], 1);
    LS_EQ_INT(q->blocks[bit_index(LS_RX_DISC)], 0);
    LS_EQ_UINT(H.slot[0].blocks, 1);
    ls_rx_head_retune(&H, 152007500);
    LS_EQ_UINT(H.stream.center_hz, 152007500);
    LS_EQ_INT(p->retunes, 1);
    ls_rx_head_reset(&H, LS_RX_RESET_USER);
    LS_EQ_INT(q->resets[LS_RX_RESET_USER], 1);
    ls_rx_head_detach_all(&H);
}

LS_CASE(no_spare_time_sheds_the_data_before_the_voice)
{
    char why[64];
    int reader = ls_decoded_open(LS_DECODED_TEXT);
    LS_CHECK(reader >= 0);
    nfm_head();
    ls_rx_head_attach(&H, &VOICE, why, sizeof(why));
    ls_rx_head_attach(&H, &PAGER, why, sizeof(why));
    fake_t *v = ls_rx_head_state(&H, &VOICE), *p = ls_rx_head_state(&H, &PAGER);
    float d[4] = {0};
    const ls_rx_block_t b = { .input = LS_RX_DISC, .data = d, .n = 4 };

    LS_CHECK(ls_rx_head_admit(&H, true));
    /* First late block: the pager is shed (with a gap) and the voice plays on. */
    LS_CHECK(ls_rx_head_admit(&H, false));
    LS_EQ_INT(p->resets[LS_RX_RESET_GAP], 1);
    ls_rx_head_feed(&H, &b);
    LS_EQ_INT(v->blocks[bit_index(LS_RX_DISC)], 1);
    LS_EQ_INT(p->blocks[bit_index(LS_RX_DISC)], 0);
    ls_rx_status_t st;
    LS_CHECK(ls_rx_head_status(&H, 1, &st));
    LS_CHECK(st.shed);
    LS_EQ_STR(st.chip, "PAGER");
    char chips[48];
    ls_rx_head_chips(&H, chips, sizeof(chips));
    LS_EQ_STR(chips, "VOICE PAGER(SHED)");
    /* Still late: now the block goes too. */
    LS_CHECK(!ls_rx_head_admit(&H, false));
    LS_EQ_UINT(H.dropped, 1);
    LS_EQ_INT(p->resets[LS_RX_RESET_GAP], 1);   /* one gap per episode */
    /* Caught up: both run again. */
    LS_CHECK(ls_rx_head_admit(&H, true));
    ls_rx_head_feed(&H, &b);
    LS_EQ_INT(p->blocks[bit_index(LS_RX_DISC)], 1);
    LS_EQ_UINT(H.shed_episodes, 1);

    ls_decoded_t ev; uint32_t lost = 0;
    LS_CHECK(ls_decoded_next(reader, &ev, &lost));
    LS_EQ_UINT(ev.kind, LS_DECODED_TEXT);
    LS_CHECK(strstr(ev.text, "SHED PAGER") != NULL);
    LS_CHECK(!ls_decoded_next(reader, &ev, &lost));

    /* Voice alone drops the block at once; data alone too. */
    ls_rx_head_detach(&H, &PAGER);
    LS_CHECK(!ls_rx_head_admit(&H, false));
    ls_rx_head_detach_all(&H);
    ls_rx_head_attach(&H, &PAGER, why, sizeof(why));
    LS_CHECK(!ls_rx_head_admit(&H, false));
    ls_rx_head_detach_all(&H);
    ls_decoded_close(reader);
}

static int s_audio_n, s_gate_open, s_tunes;
static bool audio_sink(void *u, const int16_t *pcm, int n, uint32_t rate)
{
    (void)u; (void)pcm; (void)rate;
    s_audio_n += n;
    return n > 100;   /* "dropped" */
}
static void gate_sink(void *u, bool open) { (void)u; s_gate_open = open; }
static bool tune_sink(void *u, uint32_t hz) { (void)u; s_tunes++; return hz != 0; }

LS_CASE(audio_and_tuning_go_to_the_owner)
{
    char why[64];
    nfm_head();
    ls_rx_head_set_audio(&H, audio_sink, gate_sink, NULL);
    ls_rx_head_set_tune(&H, tune_sink, NULL);
    ls_rx_head_attach(&H, &TUNER, why, sizeof(why));
    int16_t pcm[200] = {0};
    LS_CHECK(!ls_rx_audio(&H, pcm, 50, 16000));
    LS_CHECK(ls_rx_audio(&H, pcm, 150, 16000));
    LS_EQ_INT(s_audio_n, 200);
    LS_EQ_UINT(H.audio_drops, 1);
    ls_rx_audio_gate(&H, true);
    LS_EQ_INT(s_gate_open, 1);
    LS_CHECK(ls_rx_request_tune(&H, &TUNER, 152600000));
    LS_CHECK(!ls_rx_request_tune(&H, &PAGER, 152600000));   /* not a TUNES plugin */
    LS_EQ_INT(s_tunes, 1);
    ls_rx_head_detach_all(&H);
}

/* The front runs the discriminator once and hands the same samples to every
   plugin that reads them. */
LS_CASE(the_front_demodulates_once_for_every_plugin)
{
    static fm_dsp_t dsp;
    static fm_lr_dsp_t lr;
    static ls_rx_front_t front;
    char why[64];
    nfm_head();
    fm_lr_init(&lr, 25000);
    fm_dsp_init(&dsp);
    ls_rx_front_init(&front, &dsp, &lr);
    ls_rx_front_bind(&front, &H);
    LS_CHECK(ls_rx_front_dsp(&H) == &dsp);
    ls_rx_head_attach(&H, &VOICE, why, sizeof(why));
    ls_rx_head_attach(&H, &PAGER, why, sizeof(why));
    ls_rx_head_attach(&H, &DATA16, why, sizeof(why));

    uint8_t iq[1280]; double ph = 0;
    int total_disc = 0;
    for (int blk = 0; blk < 20; blk++) {
        for (int i = 0; i < 320; i++) {
            ph += 2 * M_PI * 1000.0 / 125000.0 * (1 + 0.5 * sin(i * 0.05));
            int16_t re = (int16_t)(12000 * cos(ph)), im = (int16_t)(12000 * sin(ph));
            iq[4 * i] = (uint8_t)re; iq[4 * i + 1] = (uint8_t)((uint16_t)re >> 8);
            iq[4 * i + 2] = (uint8_t)im; iq[4 * i + 3] = (uint8_t)((uint16_t)im >> 8);
        }
        total_disc += ls_rx_front_lr(&front, &H, iq, sizeof(iq), blk);
        LS_CHECK(front.nd <= LS_RX_FRONT_LR_DISC);
        LS_CHECK(front.na <= LS_RX_FRONT_LR_PCM);
    }
    fake_t *v = ls_rx_head_state(&H, &VOICE), *p = ls_rx_head_state(&H, &PAGER),
           *q = ls_rx_head_state(&H, &DATA16);
    const int D = bit_index(LS_RX_DISC), A = bit_index(LS_RX_PCM16);
    LS_CHECK(total_disc > 20 * 70);
    LS_EQ_INT(v->blocks[D], p->blocks[D]);
    LS_CHECK(v->last_data[D] == p->last_data[D]);       /* one buffer, not a copy each */
    LS_EQ_UINT(v->last_n[A], q->last_n[A]);
    LS_CHECK(v->blocks[A] > 0 && v->blocks[A] == q->blocks[A]);
    LS_CHECK(p->blocks[A] == 0);
    LS_CHECK(front.rms > 0.0f);

    /* Nothing wants the front: no demodulation at all. */
    ls_rx_head_detach_all(&H);
    LS_EQ_INT(ls_rx_front_lr(&front, &H, iq, sizeof(iq), 0), 0);
}
