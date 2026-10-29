/* WFM stereo and RDS: synthesised multiplex in, separation and a station
   card out.

   The transmitter side here is written from the same standards the decoder
   follows (ITU-R BS.450 multiplex, IEC 62106 RDS): groups are blocked with
   rds_block(), differentially encoded, biphase coded with a one-cycle sine
   per bit, and put on a 57 kHz carrier locked to the third pilot harmonic at
   an arbitrary phase. Bit timing starts at an arbitrary fraction of a bit,
   and the pilot runs off-nominal, so the PLL, the Costas loop and the
   bit-phase bank all have work to do. */
#include "ls_test.h"
#include "rds.h"
#include "wfm_mpx.h"
#include "fm_dsp.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979

/* ---- group builder ---------------------------------------------------- */

typedef struct { uint16_t b[4]; bool c2; } grp_t;

static int build_groups(grp_t *g, int max, uint16_t pi, uint8_t pty,
                        const char *ps, const char *rt)
{
    int n = 0;
    for (int seg = 0; seg < 4 && n < max; ++seg, ++n) {
        g[n].b[0] = pi;
        g[n].b[1] = (uint16_t)((0u << 12) | (pty << 5) | (1u << 3) | (unsigned)seg);
        g[n].b[2] = (uint16_t)((uint16_t)(1 + 10) << 8 | (uint16_t)(1 + 25)); /* AF 88.6, 90.0 */
        g[n].b[3] = (uint16_t)((uint8_t)ps[seg * 2] << 8 | (uint8_t)ps[seg * 2 + 1]);
        g[n].c2 = false;
    }
    char text[64];
    memset(text, ' ', sizeof(text));
    size_t len = strlen(rt);
    memcpy(text, rt, len);
    if (len < 64) text[len] = 0x0D;
    const int segs = (int)((len + 1 + 3) / 4);
    for (int seg = 0; seg < segs && n < max; ++seg, ++n) {
        g[n].b[0] = pi;
        g[n].b[1] = (uint16_t)((2u << 12) | (pty << 5) | (unsigned)seg);
        g[n].b[2] = (uint16_t)((uint8_t)text[seg * 4] << 8 | (uint8_t)text[seg * 4 + 1]);
        g[n].b[3] = (uint16_t)((uint8_t)text[seg * 4 + 2] << 8 | (uint8_t)text[seg * 4 + 3]);
        g[n].c2 = false;
    }
    /* 4A: 2026-10-28 14:37 UTC, local offset -5 h (-10 half hours).
       MJD of 2026-10-28 is 61341. */
    if (n < max) {
        const uint32_t mjd = 61341;
        const unsigned hour = 14, minute = 37;
        g[n].b[0] = pi;
        g[n].b[1] = (uint16_t)((4u << 12) | (pty << 5) | ((mjd >> 15) & 3));
        g[n].b[2] = (uint16_t)(((mjd & 0x7FFF) << 1) | (hour >> 4));
        g[n].b[3] = (uint16_t)(((hour & 15) << 12) | (minute << 6) | 0x20 | 10);
        g[n].c2 = false;
        n++;
    }
    return n;
}

/* Data bits of a run of groups, repeated `reps` times, MSB first. */
static int group_bits(const grp_t *g, int ng, int reps, uint8_t *bits, int max)
{
    int n = 0;
    for (int r = 0; r < reps; ++r)
        for (int i = 0; i < ng; ++i)
            for (int b = 0; b < 4; ++b) {
                const int blk = b == 2 && g[i].c2 ? RDS_BLK_C2 : b;
                const uint32_t w = rds_block(g[i].b[b], blk);
                for (int k = 25; k >= 0 && n < max; --k) bits[n++] = (uint8_t)((w >> k) & 1);
            }
    return n;
}

/* ---- data layer --------------------------------------------------------- */

LS_CASE(rds_check_word_and_offsets_round_trip)
{
    LS_EQ_UINT(rds_check(0), 0);
    LS_EQ_UINT(rds_block(0, RDS_BLK_A) & 0x3FF, 0x0FC);
    ls_rng_t rng; ls_rng_seed(&rng, 7);
    rds_t r;
    for (int i = 0; i < 200; ++i) {
        const uint16_t d = (uint16_t)ls_rng_u32(&rng);
        /* Distinct offsets must never alias for the same data. */
        LS_CHECK(rds_block(d, RDS_BLK_A) != rds_block(d, RDS_BLK_B));
        LS_CHECK(rds_block(d, RDS_BLK_C) != rds_block(d, RDS_BLK_C2));
    }
    rds_init(&r, NULL, NULL);
    LS_CHECK(!r.info.synced);
}

static uint8_t s_bits[40000];

LS_CASE(rds_bit_stream_decodes_station_card_from_mid_stream)
{
    grp_t g[40];
    const int ng = build_groups(g, 40, 0x54A8 + 1234, 9, "LAKESHRK",
                                "HELLO FROM LAKESHARK RDS TEST");
    const int nb = group_bits(g, ng, 4, s_bits, (int)sizeof(s_bits));
    rds_t r;
    rds_init(&r, NULL, NULL);
    /* Join late: eleven bits into a block, after some noise. */
    ls_rng_t rng; ls_rng_seed(&rng, 99);
    for (int i = 0; i < 37; ++i) rds_push_bit(&r, (int)(ls_rng_u32(&rng) & 1));
    for (int i = 11; i < nb; ++i) rds_push_bit(&r, s_bits[i]);
    LS_CHECK(r.info.synced);
    LS_CHECK(r.info.has_pi);
    LS_EQ_UINT(r.info.pi, 0x54A8 + 1234);
    LS_EQ_INT(r.info.pty, 9);
    LS_EQ_STR(r.info.ps, "LAKESHRK");
    LS_EQ_STR(r.info.rt, "HELLO FROM LAKESHARK RDS TEST");
    LS_CHECK(r.info.rt_complete);
    LS_CHECK(r.info.ct_valid);
    LS_EQ_INT(r.info.ct_offset_half, -10);
    /* 2026-10-28 14:37:00 UTC */
    LS_EQ_INT(r.info.ct_unix, 1793198220);
    LS_EQ_INT(r.info.af_n, 2);
    LS_EQ_UINT(rds_af_khz(r.info.af[0]), 88600);
    char cs[5];
    rds_callsign(r.info.pi, cs);
    LS_EQ_STR(cs, "WBVM");
    LS_EQ_STR(rds_pty_name(9, true), "TOP 40");
    LS_EQ_STR(rds_pty_name(9, false), "VARIED");
}

LS_CASE(rds_corrects_single_and_double_burst_errors)
{
    grp_t g[40];
    const int ng = build_groups(g, 40, 0x1234, 1, "ERRORS  ", "SOME BITS WENT BAD");
    const int nb = group_bits(g, ng, 4, s_bits, (int)sizeof(s_bits));
    /* One flipped bit in every fifth block, a two-bit burst in every
       seventh, after the first group so sync is not what is being tested. */
    for (int blk = 4; blk * 26 + 26 <= nb; ++blk) {
        if (blk % 5 == 0) s_bits[blk * 26 + 3] ^= 1;
        else if (blk % 7 == 0) { s_bits[blk * 26 + 12] ^= 1; s_bits[blk * 26 + 13] ^= 1; }
    }
    rds_t r;
    rds_init(&r, NULL, NULL);
    for (int i = 0; i < nb; ++i) rds_push_bit(&r, s_bits[i]);
    LS_CHECK(r.info.synced);
    LS_CHECK(r.info.blocks_fixed > 10);
    LS_EQ_INT(r.info.blocks_bad, 0);
    LS_EQ_STR(r.info.ps, "ERRORS  ");
    LS_EQ_STR(r.info.rt, "SOME BITS WENT BAD");
}

/* ---- multiplex synthesis ------------------------------------------------ */

typedef struct {
    double t;            /* seconds */
    double pilot_hz;
    double pilot_ph0;
    double rds_ph0;      /* carrier phase against 3 x pilot */
    double bit0;         /* bit timing offset, bits */
    const uint8_t *bits;
    int nbits;
    int last_bit_index;
    int diff_state;
    int sym;
    double l_hz, r_hz;   /* tone in each channel, 0 for silence */
    double l_amp, r_amp;
    double rds_amp, pilot_amp;
} mpx_gen_t;

static double mpx_next(mpx_gen_t *g)
{
    const double phi = 2.0 * PI_F * g->pilot_hz * g->t + g->pilot_ph0;
    const double L = g->l_hz > 0 ? g->l_amp * sin(2.0 * PI_F * g->l_hz * g->t) : 0.0;
    const double R = g->r_hz > 0 ? g->r_amp * sin(2.0 * PI_F * g->r_hz * g->t) : 0.0;
    double v = 0.9 * ((L + R) * 0.5 + (L - R) * 0.5 * sin(2.0 * phi)) + g->pilot_amp * sin(phi);
    if (g->bits && g->rds_amp > 0) {
        /* Bit clock is the pilot over 16, from the same phase. */
        const double bits_now = (phi - g->pilot_ph0) / (2.0 * PI_F) / 16.0 + g->bit0;
        const int bi = (int)floor(bits_now);
        const double frac = bits_now - bi;
        if (bi != g->last_bit_index) {
            g->last_bit_index = bi;
            const int d = bi >= 0 ? g->bits[bi % g->nbits] : 0;
            g->diff_state ^= d;
            g->sym = g->diff_state ? 1 : -1;
        }
        const double shape = sin(2.0 * PI_F * frac) * g->sym;
        v += g->rds_amp * shape * sin(3.0 * phi + g->rds_ph0);
    }
    g->t += 1.0 / WFM_MPX_RATE;
    return v;
}

static double goertzel(const int16_t *x, int n, double f, double fs)
{
    const double w = 2.0 * PI_F * f / fs, c = 2.0 * cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

static wfm_mpx_t s_mpx;
static int16_t s_disc[4096];
static int16_t s_l[WFM_MPX_OUT * 3], s_r[WFM_MPX_OUT * 3];

/* Runs `seconds` of the generator through the decoder; returns 16 kHz
   samples written to s_l/s_r. */
static int run_mpx(mpx_gen_t *g, double seconds, bool stereo_out)
{
    const int total = (int)(seconds * WFM_MPX_RATE);
    int out = 0;
    for (int done = 0; done < total; done += 4096) {
        int n = total - done < 4096 ? total - done : 4096;
        for (int i = 0; i < n; ++i) {
            double v = mpx_next(g) * WFM_MPX_FULL;
            s_disc[i] = (int16_t)lrint(v);
        }
        const int cap = (int)(sizeof(s_l) / sizeof(s_l[0])) - out;
        out += wfm_mpx_process(&s_mpx, s_disc, n, s_l + out, stereo_out ? s_r + out : NULL, cap);
    }
    return out;
}

static double separation_db(bool tone_left)
{
    mpx_gen_t g = {0};
    g.pilot_hz = 19000.0 + 1.3;
    g.pilot_ph0 = 0.4;
    g.pilot_amp = 0.09;
    if (tone_left) { g.l_hz = 1000; g.l_amp = 0.9; }
    else           { g.r_hz = 1000; g.r_amp = 0.9; }
    wfm_mpx_init(&s_mpx);
    const int n = run_mpx(&g, 1.5, true);
    LS_CHECK(s_mpx.pilot);
    LS_CHECK(s_mpx.stereo);
    const int skip = WFM_MPX_OUT / 2;
    const double pl = goertzel(s_l + skip, n - skip, 1000, WFM_MPX_OUT);
    const double pr = goertzel(s_r + skip, n - skip, 1000, WFM_MPX_OUT);
    return tone_left ? 10.0 * log10(pl / (pr + 1e-9)) : 10.0 * log10(pr / (pl + 1e-9));
}

LS_CASE(wfm_stereo_locks_pilot_and_separates_channels)
{
    const double left = separation_db(true);
    const double right = separation_db(false);
    ls_note("separation L %.1f dB, R %.1f dB", left, right);
    LS_CHECK_MSG(left > 30.0, "left only separation %.1f dB", left);
    LS_CHECK_MSG(right > 30.0, "right only separation %.1f dB", right);
}

LS_CASE(wfm_mono_broadcast_stays_mono_and_never_claims_stereo)
{
    mpx_gen_t g = {0};
    g.pilot_hz = 19000.0;
    g.pilot_amp = 0.0;
    g.l_hz = g.r_hz = 1000; g.l_amp = g.r_amp = 0.9;
    wfm_mpx_init(&s_mpx);
    const int n = run_mpx(&g, 1.0, true);
    LS_CHECK(!s_mpx.pilot);
    LS_CHECK(!s_mpx.stereo);
    double diff = 0, ref = 0;
    for (int i = n / 2; i < n; ++i) {
        diff += fabs((double)s_l[i] - s_r[i]);
        ref += fabs((double)s_l[i]);
    }
    LS_CHECK(ref > 0);
    LS_CHECK(diff < ref * 1e-3);
}

LS_CASE(wfm_mono_downmix_matches_the_sum)
{
    /* The speaker gets (L+R)/2: a left-only tone arrives at half level. */
    mpx_gen_t g = {0};
    g.pilot_hz = 19000.0; g.pilot_amp = 0.09;
    g.l_hz = 1000; g.l_amp = 0.9;
    wfm_mpx_init(&s_mpx);
    int n = run_mpx(&g, 1.0, false);
    double rms = 0;
    for (int i = n / 2; i < n; ++i) rms += (double)s_l[i] * s_l[i];
    rms = sqrt(rms / (n - n / 2));
    /* 0.9 * 0.9 / 2 of full deviation, less de-emphasis at 1 kHz (-0.8 dB)
       and CIC droop: the right order of loudness, not silence or clipping. */
    const double want = 0.9 * 0.9 * 0.5 * WFM_MPX_FULL / sqrt(2.0);
    LS_CHECK_MSG(rms > want * 0.6 && rms < want * 1.2, "mono rms %.0f want ~%.0f", rms, want);
}

LS_CASE(wfm_rds_decodes_through_the_multiplex)
{
    grp_t gr[40];
    const int ng = build_groups(gr, 40, 0x54A8 + 1234, 9, "LAKESHRK",
                                "STEREO AND RDS ON THE BENCH");
    static uint8_t bits[20000];
    const int nb = group_bits(gr, ng, 1, bits, (int)sizeof(bits));
    mpx_gen_t g = {0};
    g.pilot_hz = 19000.0 - 0.8;
    g.pilot_ph0 = 1.1;
    g.pilot_amp = 0.09;
    g.rds_amp = 0.04;
    g.rds_ph0 = 0.7;
    g.bit0 = 0.37;
    g.bits = bits; g.nbits = nb;
    g.last_bit_index = -1000;
    g.l_hz = 700; g.l_amp = 0.6;
    g.r_hz = 2300; g.r_amp = 0.6;
    wfm_mpx_init(&s_mpx);
    /* Enough for the card to come round twice. */
    const double seconds = 2.5 * nb / 1187.5 + 0.5;
    for (double t = 0; t < seconds; t += 1.0) run_mpx(&g, seconds - t > 1.0 ? 1.0 : seconds - t, false);
    const rds_info_t *in = &s_mpx.rds.info;
    ls_note("rds groups %lu ok %lu fixed %lu bad %lu", (unsigned long)in->groups,
            (unsigned long)in->blocks_ok, (unsigned long)in->blocks_fixed,
            (unsigned long)in->blocks_bad);
    LS_CHECK(s_mpx.stereo);
    LS_CHECK(in->synced);
    LS_EQ_UINT(in->pi, 0x54A8 + 1234);
    LS_EQ_STR(in->ps, "LAKESHRK");
    LS_EQ_STR(in->rt, "STEREO AND RDS ON THE BENCH");
    LS_CHECK(in->ct_valid);
}

LS_CASE(wfm_rds_decodes_on_a_mono_station_without_pilot)
{
    /* No pilot to lock to: the NCO free-runs at 19 kHz while the station's
       57 kHz sits slightly off it, so the Costas loop takes up the carrier
       offset and the bit-phase bank follows the slow clock drift. */
    grp_t gr[40];
    const int ng = build_groups(gr, 40, 0x1A2B, 1, "MONO FM ", "NO PILOT HERE");
    static uint8_t bits[20000];
    const int nb = group_bits(gr, ng, 1, bits, (int)sizeof(bits));
    mpx_gen_t g = {0};
    g.pilot_hz = 19000.0 + 0.4;
    g.pilot_amp = 0.0;
    g.rds_amp = 0.04; g.rds_ph0 = 0.3; g.bit0 = 0.5;
    g.bits = bits; g.nbits = nb; g.last_bit_index = -1000;
    g.l_hz = g.r_hz = 900; g.l_amp = g.r_amp = 0.7;
    wfm_mpx_init(&s_mpx);
    const double seconds = 3.0 * nb / 1187.5 + 0.5;
    for (double t = 0; t < seconds; t += 1.0) run_mpx(&g, seconds - t > 1.0 ? 1.0 : seconds - t, false);
    LS_CHECK(!s_mpx.stereo);
    LS_EQ_STR(s_mpx.rds.info.ps, "MONO FM ");
    LS_EQ_STR(s_mpx.rds.info.rt, "NO PILOT HERE");
}

LS_CASE(wfm_rds_decodes_from_fm_modulated_iq)
{
    /* The whole receive chain: the multiplex frequency-modulates a carrier
       at 75 kHz deviation, quantised to the dongle's 8-bit I/Q, then
       fm_demod_disc and the multiplex decoder. */
    grp_t gr[40];
    const int ng = build_groups(gr, 40, 0x2211, 10, "ROUNDTRP", "IQ IN STATION CARD OUT");
    static uint8_t bits[20000];
    const int nb = group_bits(gr, ng, 1, bits, (int)sizeof(bits));
    mpx_gen_t g = {0};
    g.pilot_hz = 19000.0; g.pilot_amp = 0.09;
    g.rds_amp = 0.04; g.rds_ph0 = 2.0; g.bit0 = 0.8;
    g.bits = bits; g.nbits = nb; g.last_bit_index = -1000;
    g.l_hz = 440; g.l_amp = 0.5;
    static fm_dsp_t dsp;
    fm_dsp_init(&dsp);
    wfm_mpx_init(&s_mpx);
    static uint8_t iq[8192];
    double ph = 0;
    ls_rng_t rng; ls_rng_seed(&rng, 5);
    const int blocks = (int)((2.5 * nb / 1187.5 + 0.5) * WFM_MPX_RATE / 4096);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < 4096; ++i) {
            const double v = mpx_next(&g);
            ph += 2.0 * PI_F * 75000.0 * v / WFM_MPX_RATE;
            const double noise_i = ((int)(ls_rng_u32(&rng) % 7) - 3) * 0.5;
            const double noise_q = ((int)(ls_rng_u32(&rng) % 7) - 3) * 0.5;
            iq[2 * i]     = (uint8_t)lrint(127.5 + 80.0 * cos(ph) + noise_i);
            iq[2 * i + 1] = (uint8_t)lrint(127.5 + 80.0 * sin(ph) + noise_q);
        }
        const int nd = fm_demod_disc(&dsp, iq, 8192, s_disc, 4096);
        wfm_mpx_process(&s_mpx, s_disc, nd, s_l, NULL, WFM_MPX_OUT);
    }
    const rds_info_t *in = &s_mpx.rds.info;
    LS_CHECK(s_mpx.stereo);
    LS_EQ_STR(in->ps, "ROUNDTRP");
    LS_EQ_STR(in->rt, "IQ IN STATION CARD OUT");
    LS_EQ_INT(in->pty, 10);
}
