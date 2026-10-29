/* LS_TEST_SOURCES: lakeshark/apps/rec/rec_ism.c with rec_fsk.c and
   experiments/lr433_dec.c. The recordings LR433 is tested with
   (exp_lr433_vectors.h) are put back on air as an SDR would hear them -
   unsigned 8-bit I/Q at REC's 256 kS/s, off-centre, with noise - and run
   through the ISM front end: every one must decode to what the LR2021 path
   decodes from the same recording. Also the WATCH path (edges in), noise,
   and the sink's duplicate window. */

#include "ls_test.h"

#include "experiments/lr433_dec.h"
#include "rec_ism.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exp_lr433_vectors.h"

#define N_VECS ((int)(sizeof(VECS) / sizeof(VECS[0])))
#define FS 256000u
#define EDGES 4096

static lr433_work_t W;
static rec_ism_work_t IW;
static uint8_t s_frame[256];
static int32_t s_ook[EDGES], s_fsk[EDGES];

static lr433_capture_t capture_of(const lr433_vec_t *v)
{
    memset(s_frame, 0, sizeof(s_frame));
    memcpy(s_frame, v->data, (size_t)v->len);
    const lr433_capture_t c = {
        .kind = v->kind, .rate = v->rate, .lead = v->lead, .lead_bits = v->lead_bits,
        .data = s_frame, .bits = v->bits,
    };
    return c;
}

static int level_at(const lr433_capture_t *c, int k)
{
    if (k < c->lead_bits) return (int)((c->lead >> (c->lead_bits - 1 - k)) & 1u);
    k -= c->lead_bits;
    return (c->data[k >> 3] >> (7 - (k & 7))) & 1;
}

/* ------------------------------------------------------------- the air -- */

static uint32_t s_rng = 12345u;
static int noise(int amp)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (int)((s_rng >> 16) % (unsigned)(2 * amp + 1)) - amp;
}

static uint8_t *s_iq;
static size_t s_iq_len, s_iq_cap;

static void put(double i, double q)
{
    if (s_iq_len + 2 > s_iq_cap) {
        s_iq_cap = s_iq_cap ? s_iq_cap * 2 : 1 << 20;
        s_iq = realloc(s_iq, s_iq_cap);
    }
    int a = (int)lround(127.0 + i) + noise(3), b = (int)lround(127.0 + q) + noise(3);
    s_iq[s_iq_len++] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
    s_iq[s_iq_len++] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

static void quiet(double seconds)
{
    for (long n = lround(seconds * FS); n > 0; n--) put(0, 0);
}

/* The vector's waveform: OOK keys a carrier 12 kHz off centre, FSK holds
   it and moves it by +-dev Hz around 5 kHz off. */
static void transmit(const lr433_vec_t *v, double dev_hz)
{
    const lr433_capture_t c = capture_of(v);
    const int total = c.lead_bits + c.bits;
    const double amp = 60.0;
    static double ph;
    const long samples = lround((double)total * FS / v->rate);
    for (long s = 0; s < samples; s++) {
        const int k = (int)((double)s * v->rate / FS);
        const int bit = level_at(&c, k < total ? k : total - 1);
        if (v->kind == LR433_IN_FSK) {
            ph += 2 * M_PI * (5000.0 + (bit ? dev_hz : -dev_hz)) / FS;
            put(amp * cos(ph), amp * sin(ph));
        } else {
            ph += 2 * M_PI * 12000.0 / FS;
            put(bit ? amp * cos(ph) : 0, bit ? amp * sin(ph) : 0);
        }
    }
}

/* ------------------------------------------------------------- the ear -- */

static lr433_msg_t s_heard[32];
static int s_n_heard, s_bursts;

static void burst(void *ctx, const int32_t *ook, int n_ook, const int32_t *fsk, int n_fsk, int peak)
{
    (void)ctx; (void)peak;
    s_bursts++;
    lr433_msg_t m[8];
    int k = rec_ism_decode_edges(ook, n_ook, false, &IW, m, 8);
    k += rec_ism_decode_edges(fsk, n_fsk, true, &IW, m + k, 8 - k);
    for (int i = 0; i < k && s_n_heard < 32; i++) s_heard[s_n_heard++] = m[i];
}

static void listen(void)
{
    rec_ism_front_t f;
    rec_ism_front_init(&f, FS, s_ook, s_fsk, EDGES, burst, NULL);
    s_n_heard = s_bursts = 0;
    for (size_t at = 0; at < s_iq_len; at += 8192)
        rec_ism_front_u8(&f, s_iq + at, s_iq_len - at < 8192 ? s_iq_len - at : 8192);
}

static bool same_reading(float a, float b) { return (isnan(a) && isnan(b)) || fabsf(a - b) < 0.05f; }

static bool matches(const lr433_msg_t *a, const lr433_msg_t *b)
{
    return a->proto == b->proto && a->channel == b->channel && !strcmp(a->id, b->id) &&
           !strcmp(a->model, b->model) && a->consumption == b->consumption &&
           same_reading(a->kpa, b->kpa) && same_reading(a->temp_c, b->temp_c) &&
           same_reading(a->humidity, b->humidity) && a->battery_ok == b->battery_ok;
}

static const lr433_vec_t *vec(const char *source)
{
    for (int i = 0; i < N_VECS; i++) if (!strcmp(VECS[i].source, source)) return &VECS[i];
    return NULL;
}

/* One recording on air through the SDR front end, against the LR path. */
static int over_the_air(const char *source)
{
    const lr433_vec_t *v = vec(source);
    LS_CHECK_MSG(v != NULL, "%s missing", source);
    if (!v) return 0;
    const lr433_capture_t c = capture_of(v);
    lr433_msg_t lr[8];
    const int n_lr = lr433_decode(&c, &W, lr, 8);
    LS_CHECK_MSG(n_lr > 0, "%s: LR path decodes nothing", source);
    s_iq_len = 0;
    quiet(0.03);
    transmit(v, v->rate > 70000 ? 38000.0 : 30000.0);
    quiet(0.05);
    listen();
    for (int i = 0; i < n_lr; i++) {
        bool found = false;
        for (int j = 0; j < s_n_heard && !found; j++) found = matches(&lr[i], &s_heard[j]);
        LS_CHECK_MSG(found, "%s: LR decoded %s %s, SDR front end did not (%d heard in %d bursts)",
                     source, lr[i].model, lr[i].id, s_n_heard, s_bursts);
    }
    return n_lr;
}

LS_CASE(tpms_over_sdr_matches_lr_ook_schrader)
{
    over_the_air("Schrader_EG53MA4_TPMS/01/gfile001");
    over_the_air("Schrader/01/gfile007");
}

LS_CASE(tpms_over_sdr_matches_lr_fsk_ford_toyota)
{
    over_the_air("Ford_TPMS/gfile124");
    over_the_air("Toyota_TPMS/02/0d5aee3_g007_433.92M_250k");
}

LS_CASE(weather_over_sdr_matches_lr_acurite_lacrosse)
{
    over_the_air("acurite/Acurite_592TXR/acurite-592txr-003");
    over_the_air("lacrosse/04/gfile001");
}

LS_CASE(weather_over_sdr_matches_lr_fsk_wh31e)
{
    over_the_air("ecowitt/WH31B/g006_915M_250k");
}

LS_CASE(ert_meters_over_sdr_match_lr_scm_scmplus_idm)
{
    over_the_air("ert/scm/01/g001_912.6M_2400k");
    over_the_air("scmplus/01/g002_912.6M_2359.3k");
    over_the_air("idm/IDM/g002_912.6M_2359.3k");
}

/* Every recording whose LR decode is an SDR-shaped burst: at most one burst
   lost in the whole set to the synthetic air. */
LS_CASE(most_recordings_survive_the_sdr_front_end)
{
    int pass = 0, total = 0;
    for (int i = 0; i < N_VECS; i++) {
        const lr433_vec_t *v = &VECS[i];
        const lr433_capture_t c = capture_of(v);
        lr433_msg_t lr[8];
        const int n_lr = lr433_decode(&c, &W, lr, 8);
        if (n_lr <= 0) continue;
        s_iq_len = 0;
        quiet(0.03);
        transmit(v, v->rate > 70000 ? 38000.0 : 30000.0);
        quiet(0.05);
        listen();
        bool all = true;
        for (int a = 0; a < n_lr; a++) {
            bool found = false;
            for (int j = 0; j < s_n_heard && !found; j++) found = matches(&lr[a], &s_heard[j]);
            all &= found;
        }
        if (!all) printf("  not over the air: %s\n", v->source);
        pass += all;
        total++;
    }
    printf("  %d of %d recordings decode identically over the SDR front end\n", pass, total);
    LS_CHECK(total >= 20);
    LS_CHECK(pass >= total - 1);
}

/* WATCH files edges, not I/Q: the same recording as edges decodes too. */
LS_CASE(watch_edges_decode_like_the_lr_path)
{
    const lr433_vec_t *v = vec("ert/scm/01/g001_912.6M_2400k");
    LS_CHECK(v != NULL);
    if (!v) return;
    const lr433_capture_t c = capture_of(v);
    static int32_t e[EDGES];
    int n = 0, prev = -1;
    uint64_t start = 0;
    const int total = c.lead_bits + c.bits;
    e[n++] = -5000;
    for (int k = 0; k <= total; k++) {
        const int bit = k < total ? level_at(&c, k) : 0;
        if (prev >= 0 && bit != prev) {
            const uint64_t end = (uint64_t)k * 1000000u / c.rate;
            e[n++] = (prev ? 1 : -1) * (int32_t)(end - start);
            start = end;
        }
        prev = bit;
    }
    e[n++] = -30000;
    lr433_msg_t lr[8], m[8];
    const int n_lr = lr433_decode(&c, &W, lr, 8);
    const int k = rec_ism_decode_edges(e, n, false, &IW, m, 8);
    LS_CHECK(n_lr == 1 && k >= 1);
    LS_CHECK(k >= 1 && matches(&lr[0], &m[0]));
    char line[48];
    rec_ism_describe(&m[0], line, sizeof(line));
    printf("  %s\n", line);
    LS_CHECK(!strncmp(line, "ERT-SCM ", 8));
}

LS_CASE(noise_on_air_decodes_as_nothing)
{
    s_iq_len = 0;
    s_rng = 777u;
    for (int b = 0; b < 30; b++) {
        quiet(0.025);
        /* random keyed carrier and random frequency hops */
        double ph = 0;
        const long len = 2000 + (long)(s_rng % 20000u);
        for (long s = 0; s < len; s++) {
            if ((s % 37) == 0) noise(1);
            const bool on = ((s_rng >> 9) & 1u) != 0;
            ph += 2 * M_PI * (on ? 30000.0 : -30000.0) / FS;
            put(on ? 60 * cos(ph) : 0, on ? 60 * sin(ph) : 0);
        }
    }
    quiet(0.05);
    listen();
    LS_CHECK(s_bursts > 0);
    LS_EQ_INT(s_n_heard, 0);
}

static int s_sunk;
static void sink(const rec_ism_msg_t *m, void *ctx) { (void)m; (*(int *)ctx)++; }

LS_CASE(sink_drops_a_message_heard_twice_within_a_second)
{
    LS_CHECK(rec_ism_subscribe(sink, &s_sunk));
    LS_CHECK(rec_ism_subscribe(sink, &s_sunk));      /* once only */
    rec_ism_msg_t m;
    memset(&m, 0, sizeof(m));
    m.m.proto = LR433_P_ERT_SCM;
    m.m.channel = -1;
    snprintf(m.m.id, sizeof(m.m.id), "12345");
    m.m.consumption = 100;
    rec_ism_publish(&m, 1000000);
    rec_ism_publish(&m, 1500000);                     /* WATCH heard it too */
    LS_EQ_INT(s_sunk, 1);
    rec_ism_publish(&m, 2600000);                     /* the next transmission */
    LS_EQ_INT(s_sunk, 2);
    m.m.consumption = 101;
    rec_ism_publish(&m, 2700000);                     /* a new reading is news */
    LS_EQ_INT(s_sunk, 3);
    uint32_t pub, dup;
    rec_ism_counts(&pub, &dup);
    LS_EQ_UINT(pub, 3u);
    LS_EQ_UINT(dup, 1u);
    rec_ism_unsubscribe(sink, &s_sunk);
    m.m.consumption = 102;
    rec_ism_publish(&m, 9000000);
    LS_EQ_INT(s_sunk, 3);
}
