/* LS_TEST_SOURCES: components/lakeshark/dsp/ls_floor.c

   The per-bin noise floor behind SUB-GHZ BAND's LR I/Q view and its RSSI
   sweep, driven with synthetic rows shaped like the real ones:

   - the LR2021's 125 kHz I/Q view with nothing on the air: a hump ~60% of
     the span wide, -12 dBFS on top, -48 dBFS at the edges, one FFT a row
     (exponential power, about +/-6 dB of scatter a bin);
   - an RSSI sweep across a band whose floor ripples 20 dB with frequency,
     +/-3 dB of scatter a look.

   Build with -DLS_FLOOR_OLD to run the same cases against the detector the
   firmware had before (rec_watch_runtime.c scan_band: a hit is a reading
   over the pass's 25th percentile by the gate, an event every rise through
   it). Those cases fail; that is the bug. */

#include "ls_test.h"
#include "ls_floor.h"

#include <math.h>
#include <string.h>

/* ---- deterministic noise ------------------------------------------------ */

static uint32_t g_rng;
static void seed(uint32_t s) { g_rng = s ? s : 1; }
static float uni(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return ((g_rng >> 8) + 0.5f) / 16777216.0f;
}
static float expo(void) { return -logf(uni()); }
static float gauss(void)
{
    const float u = uni(), v = uni();
    return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

/* ---- the detector under test, or the old one ---------------------------- */

typedef struct {
    ls_floor_t f;
    bool above[LS_FLOOR_BINS_MAX];
} det_t;

static void det_init(det_t *d, const ls_floor_cfg_t *cfg)
{
    memset(d, 0, sizeof(*d));
    ls_floor_init(&d->f, cfg);
}

#ifdef LS_FLOOR_OLD
/* rec_watch_runtime.c pass_floor + scan_band, as they were. */
static float old_pass_floor(const float *row, int n)
{
    float s[LS_FLOOR_BINS_MAX];
    for (int i = 0; i < n; i++) {
        const float v = row[i];
        int j = i;
        while (j > 0 && s[j - 1] > v) { s[j] = s[j - 1]; j--; }
        s[j] = v;
    }
    return s[n / 4];
}
#endif

/* One frame; returns detections that began, fills hit[]. */
static int det_frame(det_t *d, const float *db, int n, float gate, bool *hit)
{
#ifdef LS_FLOOR_OLD
    const float lo = old_pass_floor(db, n);
    int rising = 0;
    for (int i = 0; i < n; i++) {
        const bool above = db[i] >= lo + gate;
        if (above && !d->above[i]) rising++;
        d->above[i] = above;
        hit[i] = above;
    }
    return rising;
#else
    const int rising = ls_floor_update(&d->f, db, n, gate);
    for (int i = 0; i < n; i++) hit[i] = ls_floor_hit(&d->f, i);
    return rising;
#endif
}

/* ---- the LR I/Q hump ---------------------------------------------------- */

#define IQ_N 256

/* Mean noise power in dBFS, bin by bin: flat top over the middle 60%,
   raised-cosine skirts 24 bins wide, -48 dBFS beyond. */
static float hump_db(int i)
{
    const float top = -12.0f, edge = -48.0f;
    const int a = 51, b = 204, w = 24;
    float t;
    if (i >= a && i <= b) t = 1.0f;
    else if (i < a - w || i > b + w) t = 0.0f;
    else {
        const int d = i < a ? a - i : i - b;
        t = 0.5f + 0.5f * cosf(3.14159265f * (float)d / (float)w);
    }
    return edge + (top - edge) * t;
}

/* One FFT row of noise, plus a carrier `carrier_db` over the noise mean on
   `carrier_bin` (-1 for none). */
static void iq_row(float *db, int carrier_bin, float carrier_db)
{
    for (int i = 0; i < IQ_N; i++) {
        float p = powf(10.0f, hump_db(i) / 10.0f) * expo();
        if (i == carrier_bin) p += powf(10.0f, (hump_db(i) + carrier_db) / 10.0f);
        db[i] = 10.0f * log10f(p);
    }
}

static int view_level(float v)
{
    /* As the waterfall draws a scaled row: raw 0..255, sixteen levels. */
    const int raw = (int)(v * 255.0f);
    return raw * 15 / 255;
}

static void iq_noise_only(float gate)
{
    seed(0x1234u);
    static det_t d;
    det_init(&d, &LS_FLOOR_IQ);
    int events = 0, frames_hit = 0, warm = 0, cells = 0, noisy = 0;
    float db[IQ_N], view[IQ_N];
    bool hit[IQ_N];
    for (int fr = 0; fr < 1000; fr++) {
        iq_row(db, -1, 0.0f);
        events += det_frame(&d, db, IQ_N, gate, hit);
        for (int i = 0; i < IQ_N; i++) frames_hit += hit[i];
#ifndef LS_FLOOR_OLD
        if (fr >= LS_FLOOR_IQ.learn) {
            ls_floor_view(&d.f, db, view, IQ_N, LS_FLOOR_IQ_PAD_DB,
                          LS_FLOOR_IQ_WINDOW_DB, LS_FLOOR_IQ_EDGE_DB);
            for (int i = 0; i < IQ_N; i++) {
                const int l = view_level(view[i]);
                cells++;
                if (l >= 8) warm++;      /* HEAT: yellow and up       */
                if (l > 5) noisy++;      /* past blue/cyan            */
            }
        }
#endif
    }
    LS_CHECK_MSG(events == 0, "gate %.0f: %d detections on noise alone", (double)gate, events);
    LS_CHECK_MSG(frames_hit == 0, "gate %.0f: %d bin-frames held as hits", (double)gate, frames_hit);
#ifndef LS_FLOOR_OLD
    LS_CHECK_MSG(warm == 0, "%d noise cells drawn yellow or hotter", warm);
    LS_CHECK_MSG(noisy * 100 <= cells, "%d of %d noise cells above cyan", noisy, cells);
#endif
}

LS_CASE(a_the_lr_filter_hump_alone_is_never_a_detection_and_draws_as_noise)
{
    iq_noise_only(12.0f);
    iq_noise_only(25.0f);
}

LS_CASE(b_a_20_db_carrier_on_the_hump_is_found_on_its_bin_within_a_few_rows)
{
    seed(0x5151u);
    static det_t d;
    det_init(&d, &LS_FLOOR_IQ);
    const int at = 140, on = 600, len = 20;
    int events = 0, first = -1, stray = 0;
    float db[IQ_N];
    bool hit[IQ_N];
    for (int fr = 0; fr < 1000; fr++) {
        const bool tx = fr >= on && fr < on + len;
        iq_row(db, tx ? at : -1, 20.0f);
        events += det_frame(&d, db, IQ_N, 12.0f, hit);
        for (int i = 0; i < IQ_N; i++) {
            if (!hit[i]) continue;
            if (i == at) { if (first < 0) first = fr; }
            else stray++;
        }
    }
    LS_CHECK_MSG(first >= on && first <= on + 3, "carrier first held at row %d (on at %d)", first, on);
    LS_CHECK_MSG(stray == 0, "%d bin-rows held as hits away from the carrier", stray);
    LS_EQ_INT(1, events);
#ifndef LS_FLOOR_OLD
    /* Drawn warm on its bin while it is there. */
    float view[IQ_N];
    iq_row(db, at, 20.0f);
    ls_floor_update(&d.f, db, IQ_N, 12.0f);
    ls_floor_view(&d.f, db, view, IQ_N, LS_FLOOR_IQ_PAD_DB, LS_FLOOR_IQ_WINDOW_DB, LS_FLOOR_IQ_EDGE_DB);
    LS_CHECK_MSG(view_level(view[at]) >= 8, "carrier drawn at level %d", view_level(view[at]));
#endif
}

/* ---- the RSSI sweep ----------------------------------------------------- */

#define SW_N 32

/* A floor that rises and falls 20 dB across the band. */
static float sweep_floor(int i)
{
    return -110.0f + 10.0f * sinf(6.2831853f * 3.0f * (float)i / (float)SW_N);
}

static void sweep_row(float *db, int sig_bin, float sig_db)
{
    for (int i = 0; i < SW_N; i++) {
        db[i] = sweep_floor(i) + 3.0f * gauss();
        if (i == sig_bin) db[i] = sweep_floor(i) + sig_db + 1.0f * gauss();
    }
}

static void sweep_quiet(float gate)
{
    seed(0xBEEFu);
    static det_t d;
    det_init(&d, &LS_FLOOR_SWEEP);
    int events = 0, held = 0;
    float db[SW_N];
    bool hit[SW_N];
    for (int p = 0; p < 1000; p++) {
        sweep_row(db, -1, 0.0f);
        events += det_frame(&d, db, SW_N, gate, hit);
        for (int i = 0; i < SW_N; i++) held += hit[i];
    }
    LS_CHECK_MSG(events == 0, "gate %.0f: %d detections on a quiet band", (double)gate, events);
    LS_CHECK_MSG(held == 0, "gate %.0f: %d bin-passes held as hits", (double)gate, held);
}

LS_CASE(c_a_sweep_floor_rippling_20_db_never_detects_itself)
{
    sweep_quiet(12.0f);
    sweep_quiet(25.0f);
}

static void sweep_one(float gate, float sig_db)
{
    seed(0xF00Du);
    static det_t d;
    det_init(&d, &LS_FLOOR_SWEEP);
    /* Two presses of a remote on one frequency, in a trough of the floor. */
    const int at = 13;
    int events = 0, stray = 0, on_bin = 0;
    float db[SW_N];
    bool hit[SW_N];
    for (int p = 0; p < 600; p++) {
        const bool tx = (p >= 200 && p < 202) || (p >= 400 && p < 401);
        sweep_row(db, tx ? at : -1, sig_db);
        events += det_frame(&d, db, SW_N, gate, hit);
        for (int i = 0; i < SW_N; i++) {
            if (!hit[i]) continue;
            if (i == at) on_bin++;
            else stray++;
        }
        if (p == 200) LS_CHECK_MSG(hit[at], "gate %.0f: the first press was not a hit on its pass", (double)gate);
        if (p == 400) LS_CHECK_MSG(hit[at], "gate %.0f: the second press was not a hit on its pass", (double)gate);
    }
    LS_CHECK_MSG(stray == 0, "gate %.0f: %d bin-passes held as hits away from the signal", (double)gate, stray);
    LS_CHECK_MSG(events == 2, "gate %.0f: %d detections for two presses", (double)gate, events);
    LS_CHECK(on_bin >= 3);
}

LS_CASE(d_one_real_signal_on_a_rippling_floor_is_exactly_that_hit)
{
    sweep_one(12.0f, 20.0f);
    sweep_one(25.0f, 30.0f);
}

/* The SX1262 board's 902-928 MHz sweep with nothing transmitting
   (2026-11-12, DETECT 25 dB, 32 bins): -101 dBm up to 919 MHz, then a step
   to about -62 dBm, -29 at 927.2 and -24 at 928.0, the same every pass. The
   old detector called 11 of these frequencies hits ~450 times each in 25 s.
   Steady is floor: none may be a hit, and a keyfob at 915 MHz still is. */
static float sx915_floor(int i)
{
    static const float top[11] = { -74, -70, -62, -61, -63, -62, -62, -62, -62, -29, -24 };
    return i < 21 ? -101.0f : top[i - 21];
}

LS_CASE(e_the_sx1262_915_scene_is_floor_and_a_keyfob_on_it_is_one_hit)
{
    seed(0x915u);
    static det_t d;
    det_init(&d, &LS_FLOOR_SWEEP);
    const int fob = 16;   /* 915.4 MHz */
    int events = 0, stray = 0, fob_held = 0;
    float db[SW_N];
    bool hit[SW_N];
    for (int p = 0; p < 500; p++) {
        const bool press = p >= 300 && p < 302;
        for (int i = 0; i < SW_N; i++) db[i] = sx915_floor(i) + 1.5f * gauss();
        if (press) db[fob] = -60.0f + gauss();
        events += det_frame(&d, db, SW_N, 25.0f, hit);
        for (int i = 0; i < SW_N; i++) {
            if (!hit[i]) continue;
            if (i == fob) fob_held++;
            else stray++;
        }
    }
    LS_CHECK_MSG(stray == 0, "%d bin-passes away from the keyfob held as hits", stray);
    LS_CHECK_MSG(events == 1, "%d detections for one keyfob press", events);
    LS_CHECK(fob_held >= 2);
}

/* ---- the rest of the contract ------------------------------------------- */

#ifndef LS_FLOOR_OLD
LS_CASE(the_floor_is_learned_from_the_first_frames_and_a_burst_in_them_is_not)
{
    static ls_floor_t f;
    ls_floor_init(&f, &LS_FLOOR_SWEEP);
    float row[SW_N];
    for (int p = 0; p < LS_FLOOR_SWEEP.learn; p++) {
        for (int i = 0; i < SW_N; i++) row[i] = sweep_floor(i);
        if (p == 1) row[5] += 40.0f;
        LS_EQ_INT(0, ls_floor_update(&f, row, SW_N, 12.0f));
        LS_CHECK(!ls_floor_ready(&f) || p == LS_FLOOR_SWEEP.learn - 1);
    }
    LS_CHECK(ls_floor_ready(&f));
    for (int i = 0; i < SW_N; i++) LS_NEAR(ls_floor_base(&f, i), sweep_floor(i), 0.01);
}

LS_CASE(a_carrier_that_stays_on_stays_a_hit_for_minutes)
{
    seed(7);
    static ls_floor_t f;
    ls_floor_init(&f, &LS_FLOOR_IQ);
    float db[IQ_N];
    for (int r = 0; r < 50; r++) { iq_row(db, -1, 0); ls_floor_update(&f, db, IQ_N, 12.0f); }
    /* Ten rows a second: three minutes of a steady carrier. */
    for (int r = 0; r < 1800; r++) { iq_row(db, 100, 25.0f); ls_floor_update(&f, db, IQ_N, 12.0f); }
    LS_CHECK(ls_floor_hit(&f, 100));
}

LS_CASE(a_different_row_length_starts_again)
{
    static ls_floor_t f;
    ls_floor_init(&f, &LS_FLOOR_SWEEP);
    float row[SW_N];
    for (int i = 0; i < SW_N; i++) row[i] = -100.0f;
    for (int p = 0; p < 10; p++) ls_floor_update(&f, row, SW_N, 12.0f);
    LS_CHECK(ls_floor_ready(&f));
    ls_floor_update(&f, row, 16, 12.0f);
    LS_CHECK(!ls_floor_ready(&f));
}

LS_CASE(the_iq_row_helper_flattens_the_lr_scale_in_place)
{
    seed(99);
    static ls_floor_t f;
    ls_floor_init(&f, &LS_FLOOR_IQ);
    float bins[IQ_N], db[IQ_N];
    int warm = 0;
    for (int r = 0; r < 200; r++) {
        iq_row(db, -1, 0);
        for (int i = 0; i < IQ_N; i++) bins[i] = (db[i] + 100.0f) / 100.0f;
        LS_EQ_INT(0, ls_floor_iq_row(&f, bins, IQ_N, 25.0f));
        if (r < 20) continue;
        for (int i = 0; i < IQ_N; i++) {
            LS_CHECK(bins[i] >= 0.0f && bins[i] <= 1.0f);
            if (view_level(bins[i]) >= 8) warm++;
        }
    }
    LS_EQ_INT(0, warm);
}
#endif
