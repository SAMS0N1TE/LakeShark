/* lssim seeds for the FM broadcast extras: a stereo station with RDS
   (LSSIM_WFM), a running airband scan (LSSIM_AIR), an SSTV picture half
   received (LSSIM_SSTV) and the weather channels (LSSIM_WX). The snapshots
   go through the same publish calls the receive task uses. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "apps/fm/fm_bcast.h"
#include "apps/fm/fm_state.h"
#include "apps/sstv/sstv.h"

static void seed_wfm(void)
{
    const char *v = getenv("LSSIM_WFM");
    FM.mode = FM_MODE_WFM;
    FM.freq_hz = 98500000u;
    FM.iq_level = 0.62f;
    fm_wfm_status_t s;
    memset(&s, 0, sizeof(s));
    s.running = true;
    const bool none = v && !strcmp(v, "none");
    s.pilot = s.stereo = !none;
    s.pilot_level = none ? 0.0f : 0.09f;
    s.lvl_l = 0.31f;
    s.lvl_r = 0.18f;
    s.rds_level = 0.03f;
    if (!none) {
        rds_info_t *r = &s.rds;
        r->synced = r->has_pi = true;
        r->pi = 0x54A8 + 1234;          /* WBVM */
        r->pty = 9;
        r->tp = true;
        r->music = true;
        strcpy(r->ps, "LAKESHRK");
        strcpy(r->rt, "NOW PLAYING: THE BENCH BAND - SIGNAL TO NOISE (LIVE)");
        r->rt_complete = true;
        r->af_n = 3;
        r->af[0] = 11; r->af[1] = 26; r->af[2] = 140;
        r->ct_valid = true;
        r->ct_unix = 1793198220;
        r->ct_offset_half = -10;
        r->groups = 1840;
        r->blocks_ok = 7020;
        r->blocks_fixed = 212;
        r->blocks_bad = 128;
        r->seq = 5;
    }
    /* A programme's worth of history: two channels that differ, the way
       music does in stereo, and RDS blocks with the odd bad one. */
    for (int i = 0; i < FM_WFM_HIST; ++i) {
        const float beat = 0.55f + 0.45f * sinf((float)i * 0.9f);
        s.lvl_l = 0.02f + 0.30f * beat * (0.6f + 0.4f * sinf((float)i * 0.11f));
        s.lvl_r = none ? s.lvl_l : 0.02f + 0.26f * (0.55f + 0.45f * sinf((float)i * 0.9f + 1.3f)) *
                                   (0.6f + 0.4f * cosf((float)i * 0.07f));
        if (!none) {
            s.rds.blocks_ok += 5;
            if (i % 23 == 7) s.rds.blocks_bad += 2;
            if (i % 11 == 3) s.rds.blocks_fixed += 1;
        }
        fm_wfm_publish(&s);
    }
}

/* A real scan over a made-up airband: the engine itself runs a minute and a
   half of blocks against carriers that come and go, so the band strip and
   the hit list on the render are what it would show. */
static air_scan_t s_air;
static float s_db[512];
static uint32_t s_seed = 1;

static float noise(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)((s_seed >> 8) % 400) / 100.0f;
}

static void band(uint32_t lo, int64_t now, const uint32_t *hz, const float *db,
                 const int *period_s, int n)
{
    for (int i = 0; i < 512; ++i) s_db[i] = -80.0f + noise();
    s_db[256] = -45.0f;
    for (int c = 0; c < n; ++c) {
        /* On for the first 60% of each period. */
        const int64_t per = (int64_t)period_s[c] * 1000000;
        if (per && (now % per) > per * 6 / 10) continue;
        const double off = (double)hz[c] - (double)lo;
        if (off < -128000 || off > 127000) continue;
        const int b = 256 + (int)lround(off * 512 / 256000);
        const float pk = -80.0f + db[c];
        if (b >= 0 && b < 512 && pk > s_db[b]) s_db[b] = pk;
        if (b > 0 && pk - 8 > s_db[b - 1]) s_db[b - 1] = pk - 8;
        if (b < 511 && pk - 8 > s_db[b + 1]) s_db[b + 1] = pk - 8;
    }
}

static void seed_air(void)
{
    const char *v = getenv("LSSIM_AIR");
    const bool cb = v && !strcmp(v, "cb");
    const bool idle = v && !strcmp(v, "idle");
    FM.mode = FM_MODE_AM;
    FM.freq_hz = 124350000u;
    FM.iq_level = 0.31f;
    fm_air_status_t st;
    memset(&st, 0, sizeof(st));
    if (idle) { fm_air_publish(&st, NULL, 0); return; }
    static const uint32_t AIR_HZ[] = { 124350000, 118700000, 127000000, 133525000, 120900000, 135275000 };
    static const float AIR_DB[] = { 30, 18, 24, 15, 21, 26 };
    static const int AIR_PER[] = { 20, 13, 31, 17, 23, 41 };
    static const uint32_t CB_HZ[] = { 27185000, 27025000, 27385000, 27065000 };
    static const float CB_DB[] = { 28, 16, 22, 14 };
    static const int CB_PER[] = { 15, 25, 33, 19 };
    int64_t now = 1000000;
    uint32_t lo = 0;
    air_action_t a = air_scan_start(&s_air, cb ? AIR_PRESET_CB_AM : AIR_PRESET_AIR25, now);
    if (a.tune_hz) lo = a.tune_hz;
    for (int blk = 0; blk < 2900; ++blk) {
        if (cb) band(lo, now, CB_HZ, CB_DB, CB_PER, 4);
        else band(lo, now, AIR_HZ, AIR_DB, AIR_PER, 6);
        a = air_scan_block(&s_air, s_db, 512, 256000, now);
        if (a.tune_hz) lo = a.tune_hz;
        now += 32000;
    }
    const air_scan_t *s = &s_air;
    st.now_us = now;
    st.preset = s->preset;
    st.phase = s->phase;
    st.lo_hz = s->lo_hz;
    st.listen_ch = s->phase == AIR_PH_LISTEN ? s->listen_ch : -1;
    st.listen_hz = st.listen_ch >= 0 ? air_channel_hz(s->p, s->listen_ch) : 0;
    st.nch = s->nch;
    st.win = s->win;
    st.nwin = s->nwin;
    const int k0 = s->win_first[s->win], k1 = k0 + s->win_n[s->win] - 1;
    st.win_lo_hz = air_channel_hz(s->p, s->p->list ? s->order[k0] : k0);
    st.win_hi_hz = air_channel_hz(s->p, s->p->list ? s->order[k1] : k1);
    st.carrier = s->carrier && s->phase == AIR_PH_LISTEN;
    st.held = s->held;
    st.guard_on = s->guard_on && s->guard_ch >= 0;
    st.thresh_db = s->thresh_db;
    st.sweeps = s->sweeps;
    st.sweep_ms = s->sweep_ms;
    st.lockouts = air_scan_lockout_count(s);
    st.nhits = s->nhits;
    memcpy(st.hits, s->hits, sizeof(st.hits));
    if (st.listen_hz) FM.freq_hz = st.listen_hz;
    fm_air_publish(&st, s->level, s->nch);
}

/* A picture through the real decoder: a sunset scene sent as Martin 1 at
   a slightly fast clock with some hiss, the audio cut off two thirds of
   the way down (LSSIM_SSTV=1) or let finish (LSSIM_SSTV=done). */
static uint16_t *s_pix;
static int s_rows_in;
static void sink_row(void *u, int y, const uint8_t *rgb, int w)
{
    (void)u;
    for (int x = 0; x < w; ++x)
        s_pix[y * w + x] = (uint16_t)(((rgb[3 * x] & 0xF8) << 8) | ((rgb[3 * x + 1] & 0xFC) << 3) | (rgb[3 * x + 2] >> 3));
    if (y + 1 > s_rows_in) s_rows_in = y + 1;
}

static void scene(int x, int y, int w, int h, int rgb[3])
{
    const float fx = (float)x / w, fy = (float)y / h;
    const float horizon = 0.58f;
    if (fy < horizon) {
        const float t = fy / horizon;
        rgb[0] = (int)(30 + 220 * t); rgb[1] = (int)(40 + 110 * t * t); rgb[2] = (int)(120 - 60 * t);
        const float dx = fx - 0.68f, dy = (fy - 0.42f) * h / w;
        if (dx * dx + dy * dy < 0.0065f) { rgb[0] = 255; rgb[1] = 236; rgb[2] = 150; }
        const float ridge1 = horizon - 0.30f * (1.0f - fabsf(fx - 0.25f) / 0.32f);
        const float ridge2 = horizon - 0.20f * (1.0f - fabsf(fx - 0.52f) / 0.25f);
        if (fy > ridge1 && fabsf(fx - 0.25f) < 0.32f) { rgb[0] = 52; rgb[1] = 30; rgb[2] = 70; }
        else if (fy > ridge2 && fabsf(fx - 0.52f) < 0.25f) { rgb[0] = 80; rgb[1] = 40; rgb[2] = 90; }
    } else {
        const float t = (fy - horizon) / (1.0f - horizon);
        rgb[0] = (int)(20 + 30 * (1 - t)); rgb[1] = (int)(40 + 30 * (1 - t)); rgb[2] = (int)(90 + 40 * (1 - t));
        const bool glint = fabsf(fx - 0.68f) < 0.05f + 0.10f * t && ((y / 3) % 3 == 0);
        if (glint) { rgb[0] = 250; rgb[1] = 200; rgb[2] = 120; }
    }
}

static double s_ph, s_tms;
static long s_sent;
static int16_t s_abuf[1024];
static int s_an;
static sstv_t s_dec;

static void tx_tone(double hz, double ms)
{
    s_tms += ms * (1.0 + 250e-6);
    const long until = (long)(s_tms * 16.0);
    for (; s_sent < until; ++s_sent) {
        s_ph += 2.0 * 3.14159265358979 * hz / 16000.0;
        s_seed = s_seed * 1664525u + 1013904223u;
        const double v = 0.45 * sin(s_ph) + 0.06 * ((double)((s_seed >> 8) % 2001) / 1000.0 - 1.0);
        s_abuf[s_an++] = (int16_t)(v * 32767.0);
        if (s_an == 1024) { sstv_push(&s_dec, s_abuf, s_an); s_an = 0; }
    }
}

static void seed_sstv(void)
{
    const char *v = getenv("LSSIM_SSTV");
    const bool done = v && !strcmp(v, "done");
    FM.mode = FM_MODE_LISTEN;
    FM.freq_hz = 145800000u;
    FM.squelch_open = true;
    fm_bcast_set_opt(FM_BC_SSTV, true);
    const sstv_mode_t *m = sstv_mode_by_vis(44);
    s_pix = calloc((size_t)m->width * sstv_height(m), 2);
    const sstv_sink_t sink = { NULL, sink_row, NULL, NULL };
    sstv_init(&s_dec, &sink);
    tx_tone(1500, 500);
    tx_tone(1900, 300); tx_tone(1200, 10); tx_tone(1900, 300); tx_tone(1200, 30);
    int ones = 0;
    for (int i = 0; i < 7; ++i) { const int b = (44 >> i) & 1; ones += b; tx_tone(b ? 1100 : 1300, 30); }
    tx_tone((ones & 1) ? 1100 : 1300, 30);
    tx_tone(1200, 30);
    const int lines = done ? m->lines : m->lines * 2 / 3;
    for (int line = 0; line < lines; ++line) {
        tx_tone(1200, 4.862); tx_tone(1500, 0.572);
        static const int order[3] = { 1, 2, 0 };          /* G, B, R */
        for (int c = 0; c < 3; ++c) {
            for (int x = 0; x < m->width; ++x) {
                int rgb[3];
                scene(x, line, m->width, sstv_height(m), rgb);
                tx_tone(1500.0 + rgb[order[c]] * 800.0 / 255.0, 146.432 / m->width);
            }
            tx_tone(1500, 0.572);
        }
    }
    tx_tone(done ? 1500 : 1700, done ? 600 : 2);
    if (s_an) sstv_push(&s_dec, s_abuf, s_an);
    sstv_status_t st;
    sstv_status(&s_dec, &st);
    fm_sstv_set_image(s_pix, m->width, sstv_height(m));
    fm_sstv_status_t pub;
    memset(&pub, 0, sizeof(pub));
    pub.on = true;
    pub.receiving = st.phase == SSTV_IMAGE;
    snprintf(pub.mode, sizeof(pub.mode), "%s", m->name);
    pub.w = m->width;
    pub.h = sstv_height(m);
    pub.rows = s_rows_in;
    pub.slant_ppm = st.slant_ppm;
    pub.tone_hz = 1840.0f;
    pub.level = 0.3f;
    pub.syncs_found = st.syncs_found;
    pub.syncs_missed = st.syncs_missed;
    pub.images = st.images ? st.images : 2;
    snprintf(pub.saved, sizeof(pub.saved), "sstv/SSTV_2026-10-28T14-02-11Z_MARTIN_1.bmp");
    fm_sstv_publish(&pub);
}

/* The weather channels after a FIND: WX3 strongest, an alert running with
   the speaker set to alerts only. LSSIM_SAME seeds the alert list. */
static void seed_wx(void)
{
    FM.mode = FM_MODE_SAME;
    FM.freq_hz = 162475000u;
    FM.squelch_open = true;
    fm_bcast_set_wx_audio(2);
    fm_wx_status_t w;
    memset(&w, 0, sizeof(w));
    w.channel = 2;
    w.audio = 2;
    w.alert_open = true;
    static const uint8_t LV[7] = { 9, 17, 34, 3, 12, 0, 6 };
    memcpy(w.level, LV, sizeof(LV));
    fm_wx_publish(&w);
}

void lssim_bcast_seed(void)
{
    if (getenv("LSSIM_WFM")) seed_wfm();
    if (getenv("LSSIM_AIR")) seed_air();
    if (getenv("LSSIM_SSTV")) seed_sstv();
    if (getenv("LSSIM_WX")) seed_wx();
}
