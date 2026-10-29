/* SSTV receive against a synthesised transmitter: VIS header, then a test
   card (eight colour bars over a brightness ramp) in each mode, as one
   phase-continuous tone with a little noise, through the decoder at 16 kHz.
   The transmitter's clock can run fast to make the picture slant, which
   the decoder must take out. */
#include "ls_test.h"
#include "sstv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_D 3.14159265358979

/* ---- the transmitter ----------------------------------------------------- */

typedef struct {
    double phase, t_ms, clock;   /* clock: 1 + ppm * 1e-6 of the sender     */
    long   emitted;
    int16_t buf[1024];
    int    nbuf;
    sstv_t *rx;
    ls_rng_t rng;
    float  noise;
} tx_t;

static void flush(tx_t *t)
{
    if (t->nbuf) sstv_push(t->rx, t->buf, t->nbuf);
    t->nbuf = 0;
}

static void tone(tx_t *t, double hz, double ms)
{
    t->t_ms += ms * t->clock;
    const long until = (long)llround(t->t_ms * SSTV_RATE / 1000.0);
    for (; t->emitted < until; ++t->emitted) {
        t->phase += 2.0 * PI_D * hz / SSTV_RATE;
        if (t->phase > 2.0 * PI_D) t->phase -= 2.0 * PI_D;
        double v = 0.5 * sin(t->phase);
        if (t->noise > 0) v += t->noise * ((double)(ls_rng_u32(&t->rng) % 2001) / 1000.0 - 1.0);
        t->buf[t->nbuf++] = (int16_t)lrint(v * 32767.0);
        if (t->nbuf == 1024) flush(t);
    }
}

static double hz_of(int v) { return 1500.0 + v * 800.0 / 255.0; }

static void vis(tx_t *t, uint8_t code)
{
    tone(t, 1900, 300); tone(t, 1200, 10); tone(t, 1900, 300);
    tone(t, 1200, 30);
    int ones = 0;
    for (int i = 0; i < 7; ++i) {
        const int b = (code >> i) & 1;
        ones += b;
        tone(t, b ? 1100 : 1300, 30);
    }
    tone(t, (ones & 1) ? 1100 : 1300, 30);
    tone(t, 1200, 30);
}

/* The test card: bars across, brightness falling down the picture. */
static const uint8_t BARS[8][3] = {
    {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
    {255,0,255}, {255,0,0}, {0,0,255}, {0,0,0},
};
static void card(int x, int y, int w, int h, uint8_t rgb[3])
{
    const int bar = x * 8 / w;
    const float k = 1.0f - 0.5f * (float)y / (float)h;
    for (int c = 0; c < 3; ++c) rgb[c] = (uint8_t)(BARS[bar][c] * k + 0.5f);
}
static void yuv(const uint8_t rgb[3], float *Y, float *U, float *V)
{
    *Y = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    *U = 128.0f + (rgb[2] - *Y) / 1.772f;
    *V = 128.0f + (rgb[0] - *Y) / 1.402f;
}
static int clampi(float v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f); }

static void scan(tx_t *t, const sstv_mode_t *m, double ms, int y, int what)
{
    const int w = m->width, h = sstv_height(m);
    const double px = ms / w;
    for (int x = 0; x < w; ++x) {
        uint8_t rgb[3];
        float Y, U, V;
        int v;
        switch (what) {
        case SSTV_SEG_R: card(x, y, w, h, rgb); v = rgb[0]; break;
        case SSTV_SEG_G: card(x, y, w, h, rgb); v = rgb[1]; break;
        case SSTV_SEG_B: card(x, y, w, h, rgb); v = rgb[2]; break;
        case SSTV_SEG_Y: card(x, y, w, h, rgb); yuv(rgb, &Y, &U, &V); v = clampi(Y); break;
        case SSTV_SEG_Y2: card(x, y + 1, w, h, rgb); yuv(rgb, &Y, &U, &V); v = clampi(Y); break;
        case SSTV_SEG_RY: card(x, y, w, h, rgb); yuv(rgb, &Y, &U, &V); v = clampi(V); break;
        default:          card(x, y, w, h, rgb); yuv(rgb, &Y, &U, &V); v = clampi(U); break;
        }
        tone(t, hz_of(v), px);
    }
}

static void send(tx_t *t, const sstv_mode_t *m)
{
    vis(t, m->vis);
    if (m->pre_ms > 0) tone(t, 1200, m->pre_ms);
    for (int line = 0; line < m->lines; ++line) {
        const int y = m->family == SSTV_PD ? line * 2 : line;
        const bool odd = line & 1;
        for (int s = 0; s < m->nseg; ++s) {
            const sstv_seg_t *g = &m->seg[s];
            switch (g->kind) {
            case SSTV_SEG_SYNC: tone(t, 1200, g->ms); break;
            case SSTV_SEG_GAP:  tone(t, 1500, g->ms); break;
            case SSTV_SEG_SEP:  tone(t, odd ? 2300 : 1500, g->ms); break;
            case SSTV_SEG_C:    scan(t, m, g->ms, y, odd ? SSTV_SEG_BY : SSTV_SEG_RY); break;
            default:            scan(t, m, g->ms, y, g->kind); break;
            }
        }
    }
    tone(t, 1500, 300);
    flush(t);
}

/* ---- the receiver's picture ----------------------------------------------- */

static uint8_t *s_img;
static int s_w, s_h, s_rows, s_begun, s_ended;
static bool s_complete;
static const sstv_mode_t *s_mode;

static void on_begin(void *u, const sstv_mode_t *m)
{
    (void)u;
    s_mode = m; s_w = m->width; s_h = sstv_height(m); s_begun++; s_rows = 0;
    memset(s_img, 0, SSTV_MAX_W * 512 * 3);
}
static void on_row(void *u, int y, const uint8_t *rgb, int w)
{
    (void)u;
    if (y >= 0 && y < 512 && w <= SSTV_MAX_W) memcpy(s_img + (size_t)y * SSTV_MAX_W * 3, rgb, (size_t)w * 3);
    s_rows++;
}
static void on_end(void *u, bool complete, int rows) { (void)u; (void)rows; s_ended++; s_complete = complete; }

static sstv_t s_rx;

static void receive(const sstv_mode_t *m, double ppm, float noise)
{
    if (!s_img) s_img = malloc(SSTV_MAX_W * 512 * 3);
    const sstv_sink_t sink = { on_begin, on_row, on_end, NULL };
    sstv_init(&s_rx, &sink);
    s_begun = s_ended = 0; s_mode = NULL;
    static tx_t t;
    memset(&t, 0, sizeof(t));
    t.clock = 1.0 + ppm * 1e-6;
    t.rx = &s_rx;
    t.noise = noise;
    ls_rng_seed(&t.rng, 11);
    tone(&t, 1500, 700);              /* a moment of nothing first */
    send(&t, m);
}

/* Mean absolute error against the card, away from the bar edges. */
static double card_error(void)
{
    double err = 0;
    long n = 0;
    for (int y = 2; y < s_h - 2; ++y)
        for (int x = 0; x < s_w; ++x) {
            const int edge = (x * 8) % s_w;
            if (edge < s_w / 64 + 3 || s_w - edge < s_w / 64 + 3) continue;
            uint8_t want[3];
            card(x, y, s_w, s_h, want);
            const uint8_t *got = s_img + (size_t)y * SSTV_MAX_W * 3 + x * 3;
            for (int c = 0; c < 3; ++c) { err += abs((int)got[c] - (int)want[c]); n++; }
        }
    return n ? err / n : 999;
}

/* Where the first bar ends on row y: the white-to-yellow step, where blue
   falls. */
static int first_edge(int y)
{
    const uint8_t *row = s_img + (size_t)y * SSTV_MAX_W * 3;
    for (int x = 1; x < s_w; ++x) if (row[3 * x + 2] < row[3 * (x - 1) + 2] && row[3 * x + 2] < 60) return x;
    return -1;
}

static void check_mode(const char *name, double ppm, float noise, double max_err)
{
    const sstv_mode_t *m = NULL;
    for (int i = 0; i < sstv_mode_count(); ++i) if (!strcmp(sstv_mode_at(i)->name, name)) m = sstv_mode_at(i);
    LS_CHECK(m != NULL);
    if (!m) return;
    receive(m, ppm, noise);
    LS_CHECK_MSG(s_begun == 1, "%s: VIS heard %d times", name, s_begun);
    LS_CHECK_MSG(s_mode == m, "%s: decoded as %s", name, s_mode ? s_mode->name : "nothing");
    LS_CHECK_MSG(s_ended == 1 && s_complete, "%s: ended %d complete %d", name, s_ended, s_complete);
    LS_CHECK_MSG(s_rows == sstv_height(m), "%s: %d rows of %d", name, s_rows, sstv_height(m));
    const double err = card_error();
    sstv_status_t st;
    sstv_status(&s_rx, &st);
    const int e0 = first_edge(4), e1 = first_edge(s_h - 4);
    ls_note("%-10s err %.1f  slant %+.0f ppm  syncs %d/%d  edge %d..%d", name, err,
            (double)st.slant_ppm, st.syncs_found, st.syncs_found + st.syncs_missed, e0, e1);
    LS_CHECK_MSG(err < max_err, "%s: mean error %.1f", name, err);
    LS_CHECK_MSG(e0 > 0 && abs(e1 - e0) <= 2, "%s: picture slants, edge %d -> %d", name, e0, e1);
}

LS_CASE(sstv_robot36_round_trip)  { check_mode("ROBOT 36", 0, 0.05f, 14); }
LS_CASE(sstv_robot72_round_trip)  { check_mode("ROBOT 72", 0, 0.05f, 12); }
LS_CASE(sstv_martin1_round_trip)  { check_mode("MARTIN 1", 0, 0.05f, 10); }
LS_CASE(sstv_martin2_round_trip)  { check_mode("MARTIN 2", 0, 0.05f, 12); }
LS_CASE(sstv_scottie1_round_trip) { check_mode("SCOTTIE 1", 0, 0.05f, 10); }
LS_CASE(sstv_scottie2_round_trip) { check_mode("SCOTTIE 2", 0, 0.05f, 12); }
LS_CASE(sstv_scottie_dx_round_trip) { check_mode("SCOTTIE DX", 0, 0.05f, 10); }
LS_CASE(sstv_pd90_round_trip)     { check_mode("PD 90", 0, 0.05f, 12); }
LS_CASE(sstv_pd120_round_trip)    { check_mode("PD 120", 0, 0.05f, 14); }

LS_CASE(sstv_slant_from_a_fast_clock_is_taken_out)
{
    /* 400 ppm is a cheap sound card; uncorrected it would walk the edge
       some 40 pixels across a Martin 1 picture. */
    check_mode("MARTIN 1", 400, 0.05f, 12);
    sstv_status_t st;
    sstv_status(&s_rx, &st);
    LS_CHECK_MSG(fabs(st.slant_ppm - 400.0) < 60.0, "slant estimate %.0f ppm", (double)st.slant_ppm);
    check_mode("SCOTTIE 1", -300, 0.05f, 12);
}

LS_CASE(sstv_every_vis_code_names_its_mode)
{
    for (int i = 0; i < sstv_mode_count(); ++i) {
        const sstv_mode_t *m = sstv_mode_at(i);
        LS_CHECK(sstv_mode_by_vis(m->vis) == m);
    }
    LS_CHECK(sstv_mode_by_vis(0) == NULL);
}

LS_CASE(sstv_noise_alone_never_starts_a_picture)
{
    const sstv_sink_t sink = { on_begin, on_row, on_end, NULL };
    sstv_init(&s_rx, &sink);
    s_begun = 0;
    ls_rng_t rng; ls_rng_seed(&rng, 3);
    static int16_t buf[1600];
    for (int blk = 0; blk < 600; ++blk) {          /* a minute of hiss */
        for (int i = 0; i < 1600; ++i) buf[i] = (int16_t)((int)(ls_rng_u32(&rng) % 20001) - 10000);
        sstv_push(&s_rx, buf, 1600);
    }
    LS_EQ_INT(s_begun, 0);
}
