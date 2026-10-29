/* TIME MACHINE clips: the sidecar REC can read, the round trip through the
   card, and a missed page decoded after the fact from a clip - the same
   pages the receiver would have decoded live. */
#include "ls_test.h"
#include "ls_tmach_clip.h"
#include "ls_tmach_ring.h"
#include "rec_sidecar.h"
#include "fm_dsp.h"
#include "fm_state.h"
#include "pocsag.h"
#include "pocsag_gen.h"
#include "pager_iq.h"

#include <stdlib.h>
#include <string.h>

fm_state_t FM;

static ls_tm_clip_meta_t sample_meta(void)
{
    ls_tm_clip_meta_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.rec.time, sizeof(m.rec.time), "2026-10-28T03:12:44Z");
    snprintf(m.rec.radio, sizeof(m.rec.radio), "RTL-SDR");
    m.rec.freq_hz = 152600000;
    m.rec.gain_tenths = 400;
    m.rec.sample_rate = 64000;
    m.rec.span_us = 30000000;
    snprintf(m.rec.board, sizeof(m.rec.board), "T-Display-P4");
    snprintf(m.rec.firmware, sizeof(m.rec.firmware), "2.18.1-tm");
    m.version = LS_TM_CLIP_VERSION;
    snprintf(m.iq, sizeof(m.iq), "tm0007.cs8");
    m.iq_fmt = LS_TM_FMT_CS8;
    m.src_fmt = LS_TM_FMT_CU8;
    m.decim = 4;
    m.src_rate = 256000;
    m.center_hz = 5800000000ull;     /* above 32 bits: HackRF's top end */
    m.pairs = 1920000;
    m.start_us = 120000000; m.end_us = 150000000; m.saved_us = 150100000;
    m.fix = true; m.lat = 43.444512; m.lon = -71.647301; m.alt_m = 132.5f;
    snprintf(m.decoder, sizeof(m.decoder), "POCSAG");
    m.gaps = 2;
    return m;
}

LS_CASE(sidecar_round_trips_and_rec_reads_its_own_half)
{
    ls_tm_clip_meta_t m = sample_meta(), back;
    char body[LS_TM_CLIP_JSON_MAX];
    const int n = ls_tm_clip_format(body, sizeof(body), &m);
    LS_CHECK(n > 0);
    LS_CHECK(ls_tm_clip_parse(body, &back));
    LS_EQ_STR(back.rec.time, m.rec.time);
    LS_EQ_STR(back.rec.radio, "RTL-SDR");
    LS_EQ_UINT(back.rec.freq_hz, 152600000);
    LS_EQ_INT(back.rec.gain_tenths, 400);
    LS_EQ_UINT(back.rec.sample_rate, 64000);
    LS_EQ_STR(back.rec.firmware, "2.18.1-tm");
    LS_EQ_INT(back.version, 1);
    LS_EQ_STR(back.iq, "tm0007.cs8");
    LS_EQ_UINT(back.iq_fmt, LS_TM_FMT_CS8);
    LS_EQ_UINT(back.src_fmt, LS_TM_FMT_CU8);
    LS_EQ_UINT(back.decim, 4);
    LS_EQ_UINT(back.src_rate, 256000);
    LS_CHECK(back.center_hz == 5800000000ull);
    LS_CHECK(back.pairs == 1920000);
    LS_CHECK(back.start_us == 120000000 && back.end_us == 150000000 && back.saved_us == 150100000);
    LS_CHECK(back.fix);
    LS_NEAR(back.lat, 43.444512, 1e-6);
    LS_NEAR(back.lon, -71.647301, 1e-6);
    LS_NEAR(back.alt_m, 132.5, 0.05);
    LS_EQ_STR(back.decoder, "POCSAG");
    LS_EQ_UINT(back.gaps, 2);
    /* REC's own parser, unchanged, reads the REC half. */
    rec_sidecar_t rec;
    LS_CHECK(rec_sidecar_parse(body, &rec));
    LS_EQ_UINT(rec.freq_hz, 152600000);
    LS_EQ_UINT(rec.sample_rate, 64000);
    LS_EQ_UINT(rec.span_us, 30000000);
    LS_EQ_STR(rec.board, "T-Display-P4");
    /* ...within the bytes rec_sidecar_read looks at. */
    LS_CHECK(strstr(body, "\"firmware\"") - body < REC_SIDECAR_JSON_MAX - 32);
    /* Another format, a newer version, or no data file: not a clip. */
    char other[LS_TM_CLIP_JSON_MAX];
    snprintf(other, sizeof(other), "%s", body);
    memcpy(strstr(other, "lakeshark-tmclip"), "lakeshark-xxclip", 16);
    LS_CHECK(!ls_tm_clip_parse(other, &back));
    snprintf(other, sizeof(other), "%s", body);
    strstr(other, "\"version\": 1")[11] = '9';
    LS_CHECK(!ls_tm_clip_parse(other, &back));
    LS_CHECK(!ls_tm_clip_parse("{\"radio\": \"RTL-SDR\"}", &back));
    LS_EQ_UINT(ls_tm_clip_number("/sdcard/lakeshark/tmach/tm0042.json"), 42);
    LS_EQ_UINT(ls_tm_clip_number("rec001.sub"), 0);
    char base[16];
    ls_tm_clip_base(base, sizeof(base), 7);
    LS_EQ_STR(base, "tm0007");
}

LS_CASE(upsampler_is_exact_undecimated_and_linear_between_pairs)
{
    ls_tm_up_t u;
    ls_tm_up_init(&u, LS_TM_FMT_CS8, LS_TM_FMT_CU8, 1);
    const int8_t in[6] = { -128, 127, 0, -1, 5, 9 };
    uint8_t out[6];
    size_t used = 0;
    LS_EQ_UINT(ls_tm_up_run(&u, (const uint8_t *)in, 6, out, 6, &used), 6);
    LS_EQ_UINT(used, 6);
    for (int k = 0; k < 6; k++) LS_EQ_UINT(out[k], (uint8_t)(in[k] + 128));
    ls_tm_up_init(&u, LS_TM_FMT_CS16, LS_TM_FMT_CS16, 4);
    const int16_t a[4] = { 400, -400, 800, 0 };
    int16_t o[16];
    LS_EQ_UINT(ls_tm_up_bytes(&u, 2), 32);
    LS_EQ_UINT(ls_tm_up_run(&u, (const uint8_t *)a, 8, (uint8_t *)o, 16, &used), 16);  /* room for one */
    LS_EQ_UINT(used, 4);
    LS_EQ_INT(o[0], 100); LS_EQ_INT(o[1], -100); LS_EQ_INT(o[6], 400); LS_EQ_INT(o[7], -400);
    LS_EQ_UINT(ls_tm_up_run(&u, (const uint8_t *)a + 4, 4, (uint8_t *)o, 32, &used), 16);
    LS_EQ_INT(o[0], 500); LS_EQ_INT(o[1], -300); LS_EQ_INT(o[6], 800); LS_EQ_INT(o[7], 0);
}

/* ---------------------------------------------------------- fixtures -- */

/* Continuous phase 2-FSK at +-4.5 kHz around `offset`: one page, as a pager
   transmitter sends it, quantised to the receiver's format. */
static uint8_t *pocsag_iq(unsigned fmt, uint32_t rate, int baud, int offset, double amp,
                          double noise, uint32_t ric, const char *text, size_t lead, size_t *bytes)
{
    uint8_t bits[BITS_GEN_MAX];
    const size_t nb = pocsag_build_bits(ric, 3, text, 576, bits, sizeof(bits));
    const double step = (double)baud / rate;
    const size_t pairs = lead + (size_t)ceil((nb + 16) / step) + lead;
    const unsigned pb = ls_tm_pair_bytes(fmt);
    uint8_t *iq = malloc(pairs * pb);
    ls_rng_t rng;
    ls_rng_seed(&rng, 0xC0FFEE);
    double phase = 0.3;
    for (size_t k = 0; k < pairs; k++) {
        double hz = offset;
        if (k >= lead) {
            size_t at = (size_t)((k - lead) * step);
            if (at < nb) hz += bits[at] ? 4500 : -4500;
        }
        phase = remainder(phase + 6.283185307179586 * hz / rate, 6.283185307179586);
        const double i = amp * cos(phase) + noise * ls_rng_noise(&rng);
        const double q = amp * sin(phase) + noise * ls_rng_noise(&rng);
        if (fmt == LS_TM_FMT_CU8) {
            long bi = lround(i + 128), bq = lround(q + 128);
            iq[2 * k] = (uint8_t)(bi < 0 ? 0 : bi > 255 ? 255 : bi);
            iq[2 * k + 1] = (uint8_t)(bq < 0 ? 0 : bq > 255 ? 255 : bq);
        } else {
            const int16_t vi = (int16_t)lround(i), vq = (int16_t)lround(q);
            iq[4 * k] = (uint8_t)vi; iq[4 * k + 1] = (uint8_t)((uint16_t)vi >> 8);
            iq[4 * k + 2] = (uint8_t)vq; iq[4 * k + 3] = (uint8_t)((uint16_t)vq >> 8);
        }
    }
    *bytes = pairs * pb;
    return iq;
}

/* The receiver's capture loop and the worker, as the board runs them: reads
   of `read` bytes copied into the stage, the stage drained into the ring. */
static void record(ls_tm_ring_t *r, ls_tm_stage_t *st, const ls_tm_src_t *src,
                   const uint8_t *iq, size_t bytes, size_t read, int64_t *t)
{
    const unsigned pb = ls_tm_pair_bytes(src->fmt);
    for (size_t at = 0; at < bytes; at += read) {
        const size_t n = bytes - at < read ? bytes - at : read;
        *t += (int64_t)((double)(n / pb) * 1e6 / src->rate_hz);
        ls_tm_stage_put(st, src, iq + at, n, *t);
        const uint8_t *d;
        const ls_tm_slot_meta_t *m;
        while ((m = ls_tm_stage_peek(st, &d))) { ls_tm_ring_feed(r, m, d); ls_tm_stage_release(st); }
    }
    ls_tm_stage_flush(st, true, *t);
    const uint8_t *d;
    const ls_tm_slot_meta_t *m;
    while ((m = ls_tm_stage_peek(st, &d))) { ls_tm_ring_feed(r, m, d); ls_tm_stage_release(st); }
}

/* "WHAT WAS THAT?": the window to the card, the way the worker does it. */
static bool save_clip(ls_tm_ring_t *r, const char *dir, uint32_t no, int64_t from, int64_t to,
                      ls_tm_clip_meta_t *m)
{
    ls_tm_window_t w;
    if (!ls_tm_window_begin(r, &w, from, to)) return false;
    char base[16], path[200];
    ls_tm_clip_base(base, sizeof(base), no);
    memset(m, 0, sizeof(*m));
    snprintf(m->iq, sizeof(m->iq), "%s.%s", base, ls_tm_fmt_name(w.seg.fmt));
    snprintf(path, sizeof(path), "%s/%s", dir, m->iq);
    remove(path);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t chunk[3000];
    size_t n, total = 0;
    while ((n = ls_tm_window_step(r, &w, chunk, sizeof(chunk))) > 0) { fwrite(chunk, 1, n, f); total += n; }
    fclose(f);
    ls_tm_window_end(r, &w);
    snprintf(m->rec.radio, sizeof(m->rec.radio), "%s", ls_tm_radio_name(w.seg.src.radio));
    m->rec.freq_hz = (uint32_t)w.seg.src.center_hz;
    m->rec.sample_rate = w.seg.rate_hz;
    m->rec.span_us = (uint32_t)(w.end_us - w.start_us);
    m->version = LS_TM_CLIP_VERSION;
    m->iq_fmt = w.seg.fmt; m->src_fmt = w.seg.src.fmt;
    m->decim = w.seg.decim; m->src_rate = w.seg.src.rate_hz;
    m->center_hz = w.seg.src.center_hz;
    m->pairs = total / w.seg.pair_bytes;
    m->start_us = w.start_us; m->end_us = w.end_us;
    snprintf(m->decoder, sizeof(m->decoder), "POCSAG");
    snprintf(path, sizeof(path), "%s/%s.json", dir, base);
    remove(path);
    return ls_tm_clip_write_meta(dir, base, m);
}

typedef struct {
    pocsag_ctx_t *p[2];
    fm_state_t out[2];
    fm_dsp_t dsp;
    pgr_iq_t pgr;
    bool lr;
} pager_t;

static bool pager_sink(void *ctx, const uint8_t *iq, size_t bytes)
{
    pager_t *pg = ctx;
    static float demod[8192], neg[8192];
    int nd;
    if (pg->lr) {
        size_t got = 0;
        if (!pgr_iq_process(&pg->pgr, iq, bytes, demod, 8192, &got)) return false;
        nd = (int)got;
    } else {
        nd = fm_demod_iq(&pg->dsp, iq, (int)bytes, demod, 8192);
    }
    for (int i = 0; i < nd; i++) neg[i] = -demod[i];
    pocsag_process(pg->p[0], demod, nd);
    pocsag_process(pg->p[1], neg, nd);
    return true;
}

static int pages_after_replay(const char *dir, uint32_t no, bool lr, pager_t *pg)
{
    char base[16], path[200];
    ls_tm_clip_base(base, sizeof(base), no);
    ls_tm_clip_meta_t m;
    if (!ls_tm_clip_read_meta(dir, base, &m)) return -1;
    snprintf(path, sizeof(path), "%s/%s", dir, m.iq);
    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    memset(pg, 0, sizeof(*pg));
    pg->lr = lr;
    fm_dsp_init(&pg->dsp);
    pgr_iq_init(&pg->pgr);
    for (int i = 0; i < 2; i++) {
        pg->p[i] = pocsag_create(&pg->out[i], 1200);
        if (lr) pocsag_sample_rate(pg->p[i], PGR_IQ_OUTPUT_RATE);
    }
    /* 2048 stored pairs: one fm_iq_replay block of 16384 bytes at 256k. */
    ls_tm_clip_play(f, &m, lr ? 1024 : 2048, pager_sink, pg);
    fclose(f);
    return (int)(pocsag_n_pages(pg->p[0]) + pocsag_n_pages(pg->p[1]));
}

static const fm_page_t *the_page(pager_t *pg)
{
    for (int i = 0; i < 2; i++) if (pocsag_n_pages(pg->p[i])) return &pg->out[i].pages[0];
    return NULL;
}

static const char *test_dir(void)
{
    static char dir[200];
    const char *tmp = getenv("TEMP");
    snprintf(dir, sizeof(dir), "%s", tmp ? tmp : ".");
    return dir;
}

LS_CASE(a_missed_rtl_page_decodes_from_the_clip)
{
    size_t bytes;
    /* 0.3 s of carrier either side of the page, at 256k CU8 like FM's POCSAG. */
    uint8_t *iq = pocsag_iq(LS_TM_FMT_CU8, FM_RTL_RATE, 1200, 2000, 90, 4, 1234568, "MISSED IT", 76800, &bytes);
    static uint8_t ring_mem[4 << 20];
    ls_tm_ring_t r;
    ls_tm_ring_init(&r, ring_mem, sizeof(ring_mem), 60000);
    ls_tm_stage_t st;
    uint8_t *stage = calloc(LS_TM_SLOTS, LS_TM_SLOT_BYTES);
    ls_tm_stage_init(&st, stage);
    ls_tm_src_t src = { .center_hz = 152600000, .rate_hz = FM_RTL_RATE, .gain_tenths = 400,
                        .fmt = LS_TM_FMT_CU8, .radio = LS_TM_RADIO_RTL };
    int64_t t = 50000000;
    record(&r, &st, &src, iq, bytes, 16384, &t);
    const ls_tm_seg_t *seg = ls_tm_ring_seg_newest(&r);
    LS_EQ_UINT(seg->rate_hz, 64000);                 /* a quarter of the bytes kept */
    ls_tm_clip_meta_t m;
    LS_CHECK(save_clip(&r, test_dir(), 9001, 0, 0, &m));
    LS_EQ_UINT(m.decim, 4);
    pager_t *pg = calloc(1, sizeof(*pg));
    const int pages = pages_after_replay(test_dir(), 9001, false, pg);
    LS_CHECK_MSG(pages >= 1, "pages %d frames %u/%u cwerr %u/%u", pages,
                 pocsag_n_frames(pg->p[0]), pocsag_n_frames(pg->p[1]),
                 pocsag_n_cwerr(pg->p[0]), pocsag_n_cwerr(pg->p[1]));
    const fm_page_t *pgp = the_page(pg);
    LS_CHECK(pgp != NULL);
    if (pgp) {
        LS_EQ_UINT(pgp->address, 1234568);
        LS_EQ_STR(pgp->text, "MISSED IT");
    }
    /* The live chain on the original samples decodes the same page. */
    pager_t *live = calloc(1, sizeof(*live));
    fm_dsp_init(&live->dsp);
    for (int i = 0; i < 2; i++) live->p[i] = pocsag_create(&live->out[i], 1200);
    for (size_t at = 0; at + 16384 <= bytes; at += 16384) pager_sink(live, iq + at, 16384);
    LS_EQ_UINT(pocsag_n_pages(live->p[0]) + pocsag_n_pages(live->p[1]), (unsigned)pages);
    const fm_page_t *lp = the_page(live);
    LS_CHECK(lp && pgp && !strcmp(lp->text, pgp->text) && lp->address == pgp->address);
    for (int i = 0; i < 2; i++) { pocsag_destroy(pg->p[i]); pocsag_destroy(live->p[i]); }
    free(pg); free(live); free(iq); free(stage);
}

LS_CASE(a_missed_lr2021_page_decodes_from_the_clip)
{
    size_t bytes;
    uint8_t *iq = pocsag_iq(LS_TM_FMT_CS16, PGR_IQ_INPUT_RATE, 1200, -3000, 12000, 2500, 1654320,
                            "LR CLIP", 37500, &bytes);
    static uint8_t ring_mem[4 << 20];
    ls_tm_ring_t r;
    ls_tm_ring_init(&r, ring_mem, sizeof(ring_mem), 60000);
    ls_tm_stage_t st;
    uint8_t *stage = calloc(LS_TM_SLOTS, LS_TM_SLOT_BYTES);
    ls_tm_stage_init(&st, stage);
    ls_tm_src_t src = { .center_hz = 152600000, .rate_hz = PGR_IQ_INPUT_RATE,
                        .fmt = LS_TM_FMT_CS16, .radio = LS_TM_RADIO_LR2021 };
    int64_t t = 1000000;
    record(&r, &st, &src, iq, bytes, 1024, &t);     /* the wide path's 256-pair calls */
    LS_EQ_UINT(ls_tm_ring_seg_newest(&r)->decim, 2);
    ls_tm_clip_meta_t m;
    LS_CHECK(save_clip(&r, test_dir(), 9002, 0, 0, &m));
    LS_EQ_UINT(m.iq_fmt, LS_TM_FMT_CS16);
    pager_t *pg = calloc(1, sizeof(*pg));
    const int pages = pages_after_replay(test_dir(), 9002, true, pg);
    LS_CHECK_MSG(pages >= 1, "pages %d frames %u/%u cwerr %u/%u", pages,
                 pocsag_n_frames(pg->p[0]), pocsag_n_frames(pg->p[1]),
                 pocsag_n_cwerr(pg->p[0]), pocsag_n_cwerr(pg->p[1]));
    const fm_page_t *pgp = the_page(pg);
    if (pgp) {
        LS_EQ_UINT(pgp->address, 1654320);
        LS_EQ_STR(pgp->text, "LR CLIP");
    }
    for (int i = 0; i < 2; i++) pocsag_destroy(pg->p[i]);
    free(pg); free(iq); free(stage);
}

LS_CASE(an_undecimated_clip_plays_back_byte_for_byte)
{
    size_t bytes;
    uint8_t *iq = pocsag_iq(LS_TM_FMT_CU8, 48000, 1200, 0, 60, 3, 42, "X", 1000, &bytes);
    static uint8_t ring_mem[1 << 20];
    ls_tm_ring_t r;
    ls_tm_ring_init(&r, ring_mem, sizeof(ring_mem), 60000);   /* 48k is already narrow */
    ls_tm_stage_t st;
    uint8_t *stage = calloc(LS_TM_SLOTS, LS_TM_SLOT_BYTES);
    ls_tm_stage_init(&st, stage);
    ls_tm_src_t src = { .center_hz = 433920000, .rate_hz = 48000, .fmt = LS_TM_FMT_CU8, .radio = LS_TM_RADIO_HACKRF };
    int64_t t = 0;
    record(&r, &st, &src, iq, bytes, 5000, &t);
    ls_tm_clip_meta_t m;
    LS_CHECK(save_clip(&r, test_dir(), 9003, 0, 0, &m));
    LS_EQ_UINT(m.decim, 1);
    LS_EQ_UINT(m.pairs, bytes / 2);
    char path[200];
    snprintf(path, sizeof(path), "%s/%s", test_dir(), m.iq);
    FILE *f = fopen(path, "rb");
    LS_CHECK(f != NULL);
    uint8_t *back = malloc(bytes);
    struct { uint8_t *p; size_t n; } acc = { back, 0 };
    ls_tm_up_t up;
    ls_tm_up_init(&up, m.iq_fmt, m.src_fmt, m.decim);
    uint8_t in[512], out[512];
    size_t n;
    while (f && (n = fread(in, 1, sizeof(in), f)) > 0) {
        size_t used;
        const size_t k = ls_tm_up_run(&up, in, n, out, sizeof(out), &used);
        memcpy(acc.p + acc.n, out, k);
        acc.n += k;
    }
    if (f) fclose(f);
    LS_EQ_UINT(acc.n, bytes);
    LS_CHECK(memcmp(back, iq, bytes) == 0);
    free(back); free(iq); free(stage);
}
