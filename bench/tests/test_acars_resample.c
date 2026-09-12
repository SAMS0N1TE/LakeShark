/* LS_TEST_SOURCES: ${APP}/acars/acars_resample.c ${APP}/acars/acars.c ${APP}/acars/acars_msk.c */
/* 3:5 resampler that feeds the ACARS MSK slicer.

   The FM NBFM chain produces 32 kHz audio; the slicer only understands
   19200 Hz.  32000 * 3/5 = 19200 exactly, so nothing about the arithmetic
   has to drift.  This file locks that down and, separately, drives a
   real ACARS transmission through the two-stage path (fixture at 19.2 k
   -> upsample to 32 k -> resample back to 19.2 k -> decoder) so the
   resampler cannot silently corrupt what the slicer sees. */

#include "ls_test.h"
#include "acars_gen.h"
#include "acars.h"
#include "acars_resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LS_CASE(exact_ratio_over_a_long_block)
{
    /* One 32 ms input block is 1024 samples at 32 kHz; that must produce
       614 or 615 output samples (32 ms * 19200 = 614.4) without drift.
       This is the invariant the ACARS pipeline relies on - if the
       integer phase went adrift, MSK sync would slowly walk off within
       seconds. */
    acars_rs_t rs;
    acars_rs_init(&rs);

    static float in[1024];
    for (int i = 0; i < 1024; i++) in[i] = 0.0f;
    static float out[1024];

    int total = 0;
    for (int k = 0; k < 250; k++) {           /* ~8 seconds */
        int no = acars_rs_5to3(&rs, in, 1024, out, 1024);
        total += no;
    }
    /* 250 blocks * 1024 samples = 256000 samples in.  256000 * 3/5 =
       153600 samples out - not one more, not one fewer. */
    LS_EQ_INT(total, 153600);
}

LS_CASE(constant_input_is_reproduced_faithfully)
{
    /* Linear interpolation of a constant should give exactly that
       constant back on every output sample.  This catches an off-by-one
       in the interpolation weights that would otherwise show up as a
       small DC bias to the slicer. */
    acars_rs_t rs;
    acars_rs_init(&rs);

    static float in[1024];
    for (int i = 0; i < 1024; i++) in[i] = 0.375f;
    static float out[1024];

    /* Do two passes so the primed 'last' path is exercised. */
    (void)acars_rs_5to3(&rs, in, 1024, out, 1024);
    int no = acars_rs_5to3(&rs, in, 1024, out, 1024);
    LS_CHECK_MSG(no > 0, "second pass produced no samples");
    for (int i = 0; i < no; i++)
        LS_NEAR(out[i], 0.375f, 1e-5);
}

LS_CASE(tone_at_2400_survives_the_downsample)
{
    /* The ACARS space tone is 2400 Hz.  Feed it in at 32 kHz and check
       the peak of the resampled block is still near unity - if the
       interpolator was doing something crooked (dropping samples,
       rounding phase wrong), 2400 Hz would fold or attenuate visibly. */
    acars_rs_t rs;
    acars_rs_init(&rs);

    static float in[1024];
    for (int i = 0; i < 1024; i++)
        in[i] = (float)sin(2.0 * M_PI * 2400.0 * (double)i / 32000.0);

    static float out[1024];
    int no = acars_rs_5to3(&rs, in, 1024, out, 1024);
    LS_CHECK_MSG(no >= 600, "resampler emitted only %d samples", no);

    float peak = 0.0f;
    for (int i = 0; i < no; i++) {
        float a = out[i]; if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    LS_CHECK_MSG(peak > 0.92f,
        "2400 Hz peak collapsed to %.3f - resampler is filtering the signal", peak);
}

LS_CASE(phase_survives_block_boundaries)
{
    /* Emit the same 32-kHz stream in two different chunkings and check
       the two output streams agree bit-for-bit after the resampler's
       transient.  If block-boundary state (last, pos_num) is wrong, the
       two runs will diverge - which on the wire looks like a bit slip
       every few 32 ms buffers. */
    static float in[2048];
    for (int i = 0; i < 2048; i++)
        in[i] = (float)sin(2.0 * M_PI * 1200.0 * (double)i / 32000.0);

    static float a[2048], b[2048];

    acars_rs_t r1; acars_rs_init(&r1);
    int na = acars_rs_5to3(&r1, in, 2048, a, 2048);

    acars_rs_t r2; acars_rs_init(&r2);
    int nb1 = acars_rs_5to3(&r2, in,        1024, b,         2048);
    int nb2 = acars_rs_5to3(&r2, in + 1024, 1024, b + nb1,   2048 - nb1);
    int nb  = nb1 + nb2;

    LS_EQ_INT(na, nb);
    /* First few outputs after init use the un-primed branch on both
       runs, so they should agree exactly.  Compare the whole span. */
    for (int i = 0; i < na; i++)
        LS_NEAR(a[i], b[i], 1e-5);
}

LS_CASE(real_acars_message_survives_the_round_trip)
{
    /* End-to-end: transmit a real message at 19.2 kHz, upsample it to
       32 kHz (the rate the FM path would deliver), run it back through
       the 3:5 resampler and hand the result to acars_process().  If the
       resampler produces something a wire ACARS decoder can decode, the
       device path will too. */
    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);
    acars_msg_t m;
    memset(&m, 0, sizeof(m));
    m.mode     = '2';
    m.tak      = 0x15;
    m.block_id = '1';
    snprintf(m.reg,   sizeof(m.reg),   "%-7s", ".N12345");
    snprintf(m.label, sizeof(m.label), "%s",   "H1");
    m.text = "RESAMPLED PATH OK";

    /* Wire fixture at 19.2 kHz. */
    static float wire[40000];
    size_t n_wire = acars_tx_msg(&cfg, &m, wire, 40000);
    LS_CHECK(n_wire > 0);

    /* Naive linear up-sample 19.2 -> 32 kHz - this stands in for what
       the RF path delivers.  ratio_in = 19200/32000 = 3/5. */
    static float up32[80000];
    int n_up = 0;
    for (int k = 0; k < 80000; k++) {
        double t = (double)k * 3.0 / 5.0;    /* position in wire samples */
        int   i  = (int)t;
        double f = t - (double)i;
        if (i + 1 >= (int)n_wire) break;
        up32[k]  = (float)((1.0 - f) * wire[i] + f * wire[i + 1]);
        n_up = k + 1;
    }

    /* Now the actual path under test: 3:5 resample down and decode. */
    acars_rs_t rs;
    acars_rs_init(&rs);
    static float down[80000];
    int n_down = 0;
    for (int off = 0; off < n_up; off += 1024) {
        int take = (n_up - off < 1024) ? (n_up - off) : 1024;
        int got  = acars_rs_5to3(&rs, up32 + off, take,
                                 down + n_down, 80000 - n_down);
        n_down += got;
    }

    acars_state_t state;
    memset(&state, 0, sizeof(state));
    acars_ctx_t *c = acars_create(&state);
    LS_CHECK(c != NULL);

    for (int off = 0; off < n_down; off += 1024) {
        int take = (n_down - off < 1024) ? (n_down - off) : 1024;
        acars_process(c, down + off, take);
    }

    LS_CHECK_MSG(state.msg_count > 0,
        "message did not survive the 32k round-trip");
    if (state.msg_count > 0) {
        int last = (state.msg_head - 1 + ACARS_MSG_LOG_MAX) % ACARS_MSG_LOG_MAX;
        LS_EQ_STR(state.msgs[last].text, "RESAMPLED PATH OK");
        LS_EQ_STR(state.msgs[last].reg,  ".N12345");
    }

    acars_destroy(c);
}
