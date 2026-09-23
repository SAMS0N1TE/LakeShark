#include "ls_test.h"
#include "fm_dsp.h"
#include <math.h>
#include <string.h>

static fm_dsp_t dsp;
static uint8_t iq[4096];
static int16_t audio[8192];

/* Bench measurements on a T-Display-P4 with an RTL-SDR, demod_noise as the
   code sees it: a strong local broadcast 0.28..0.59, dead air 0.81..0.97.
   The cases use those numbers rather than invented ones, because the defect
   this replaced was a test that drove iq_block_peak at 0.30 and 1.00 while
   the hardware never produced more than 0.07. */
#define NFM_SIGNAL  0.47f
#define NFM_HISS    0.90f

LS_CASE(nfm_squelch_rejects_retune_and_short_spikes)
{
    fm_dsp_init(&dsp);
    dsp.demod_noise = NFM_SIGNAL;
    /* The settle window after a retune holds it shut whatever it hears. */
    LS_CHECK(!fm_nfm_squelch(&dsp, 30, 1024));
    LS_CHECK(!fm_nfm_squelch(&dsp, 30, 1024));
    LS_CHECK(!fm_nfm_squelch(&dsp, 30, 1024));
    dsp.demod_noise = NFM_HISS;
    LS_CHECK(!fm_nfm_squelch(&dsp, 30, 1024));
    dsp.demod_noise = NFM_SIGNAL;
    LS_CHECK(!fm_nfm_squelch(&dsp, 30, 1024));
    LS_CHECK(fm_nfm_squelch(&dsp, 30, 1024));
    /* Hiss now has to be sustained to shut it, so one block does not. */
    dsp.demod_noise = NFM_HISS;
    LS_CHECK(fm_nfm_squelch(&dsp, 30, 1024));
    fm_dsp_init(&dsp);
    dsp.demod_noise = NFM_SIGNAL;
    LS_CHECK(!fm_nfm_squelch(&dsp, 30, 1024));
}

LS_CASE(one_noisy_block_inside_a_transmission_does_not_chop_the_audio)
{
    /* The chop, as a case. A solid signal with an occasional noisy block used
       to shut the squelch on that block and then need another 64 ms qualify
       window to reopen, and the player splices a silence in for every gap.
       Hardware showed about 1.6 of those a second on a clean broadcast. */
    fm_dsp_init(&dsp);
    dsp.squelch_settle_samples = 0;
    dsp.demod_noise = NFM_SIGNAL;
    int open = 0;
    for (int i = 0; i < 8; i++) open = fm_nfm_squelch(&dsp, 30, 1024);
    LS_CHECK(open);

    for (int i = 0; i < 6; i++) {
        dsp.demod_noise = NFM_HISS;            /* one bad block */
        LS_CHECK_MSG(fm_nfm_squelch(&dsp, 30, 1024),
                     "a single noisy block shut the squelch at spike %d", i);
        dsp.demod_noise = NFM_SIGNAL;          /* and the signal is back */
        LS_CHECK(fm_nfm_squelch(&dsp, 30, 1024));
    }
}

LS_CASE(sustained_hiss_still_closes_it)
{
    /* Hysteresis must not become a latch: when the transmission really ends,
       the squelch has to shut rather than hold the hiss open. */
    fm_dsp_init(&dsp);
    dsp.squelch_settle_samples = 0;
    dsp.demod_noise = NFM_SIGNAL;
    int open = 0;
    for (int i = 0; i < 8; i++) open = fm_nfm_squelch(&dsp, 30, 1024);
    LS_CHECK(open);

    dsp.demod_noise = NFM_HISS;
    for (int i = 0; i < 40 && open; i++) open = fm_nfm_squelch(&dsp, 30, 1024);
    LS_CHECK_MSG(!open, "sustained hiss never closed the squelch");
}

LS_CASE(the_default_gate_sits_between_measured_signal_and_measured_hiss)
{
    /* The whole defect in one case: at the shipped default, a real signal
       must open and real dead air must not. */
    fm_dsp_init(&dsp);
    dsp.squelch_settle_samples = 0;
    dsp.demod_noise = NFM_SIGNAL;
    int open = 0;
    for (int i = 0; i < 8; i++) open = fm_nfm_squelch(&dsp, 30, 1024);
    LS_CHECK_MSG(open, "a signal at %.2f stays muted at the default gate",
                 (double)NFM_SIGNAL);

    fm_dsp_init(&dsp);
    dsp.squelch_settle_samples = 0;
    dsp.demod_noise = NFM_HISS;
    for (int i = 0; i < 8; i++) open = fm_nfm_squelch(&dsp, 30, 1024);
    LS_CHECK_MSG(!open, "dead air at %.2f opens the squelch at the default gate",
                 (double)NFM_HISS);
}

LS_CASE(the_control_still_runs_the_right_way_round)
{
    /* Higher is more squelch. 0 opens on anything, 100 opens on nothing. */
    fm_dsp_init(&dsp);
    dsp.squelch_settle_samples = 0;
    dsp.demod_noise = NFM_HISS;
    int open = 0;
    for (int i = 0; i < 8; i++) open = fm_nfm_squelch(&dsp, 0, 1024);
    LS_CHECK_MSG(open, "squelch 0 should pass even hiss");

    fm_dsp_init(&dsp);
    dsp.squelch_settle_samples = 0;
    dsp.demod_noise = 0.0f;
    for (int i = 0; i < 8; i++) open = fm_nfm_squelch(&dsp, 100, 1024);
    LS_CHECK_MSG(!open, "squelch 100 should pass nothing");
}

LS_CASE(nfm_current_level_does_not_retain_previous_peak)
{
    static float demod[1100];
    fm_dsp_init(&dsp);
    memset(iq, 250, sizeof(iq));
    fm_demod_iq(&dsp, iq, sizeof(iq), demod, 1100);
    LS_CHECK(dsp.iq_block_peak > 0.9f);
    memset(iq, 128, sizeof(iq));
    fm_demod_iq(&dsp, iq, sizeof(iq), demod, 1100);
    LS_CHECK(dsp.iq_block_peak < 0.02f);
    LS_CHECK(dsp.iq_peak > 0.9f);
}

/* An FM discriminator reads phase, so how loud the carrier is must not change
   what comes out of it.  That is the property this pins, and it is the one
   that broke: fast_atan2_i multiplied by 4096 in an int, so once the dot
   product of two consecutive IQ samples passed INT_MAX/4096 the product
   wrapped and the angle came back wrong - on strong signals only, which is
   why it survived every quiet bench run.

   The two amplitudes are chosen to sit either side of that: at 20 the
   narrowband chain's three interpolation passes leave the product well
   inside an int, and at 120 they do not. */
static float tone_at(int amplitude, double step)
{
    fm_dsp_init(&dsp);
    static float demod[4096];
    double total = 0;
    int counted = 0;
    for (int block = 0; block < 4; ++block) {
        for (int i = 0; i < 2048; ++i) {
            const double phase = (block * 2048 + i) * step;
            iq[i * 2]     = (uint8_t)lround(127 + amplitude * cos(phase));
            iq[i * 2 + 1] = (uint8_t)lround(127 + amplitude * sin(phase));
        }
        const int n = fm_demod_iq(&dsp, iq, sizeof(iq), demod, 4096);
        /* The first block carries the filter histories' settling. */
        if (block == 0) continue;
        for (int i = 0; i < n; ++i) { total += demod[i]; counted++; }
    }
    return counted ? (float)(total / counted) : 0.0f;
}

LS_CASE(nfm_demod_does_not_depend_on_how_loud_the_carrier_is)
{
    /* Eight input samples per output sample, so a 0.03125 rad step per input
       sample is a 0.25 rad step per output sample. */
    const double step = 0.25 / 8.0;
    const float quiet = tone_at(20, step);
    const float loud  = tone_at(120, step);

    /* The band, not the number: fast_atan2_i is a piecewise approximation
       and reads a true 0.25 rad as about 0.32, so pinning it to atan2 would
       be pinning its error rather than its behaviour.  What has to hold is
       that it lands somewhere sane at all - a wrapped multiply does not. */
    LS_CHECK(quiet > 0.15f && quiet < 0.50f);
    LS_CHECK(loud  > 0.15f && loud  < 0.50f);

    /* The actual regression: same phase step, different carrier level, same
       answer. */
    LS_NEAR(quiet, loud, 0.01f);
}

static int receive(float depth)
{
    fm_dsp_init(&dsp);
    int total = 0;
    for (int block = 0; block < 64; ++block) {
        for (int i = 0; i < 2048; ++i) {
            const double t = (block * 2048 + i) / 256000.0;
            const double amplitude = 64 * (1 + depth * sin(6.28318530718 * 1000 * t));
            iq[i * 2] = (uint8_t)lround(127 + amplitude * cos(6.28318530718 * 4000 * t));
            iq[i * 2 + 1] = (uint8_t)lround(127 + amplitude * sin(6.28318530718 * 4000 * t));
        }
        total += fm_demod_am(&dsp, iq, sizeof(iq), audio + total, 8192 - total);
    }
    return total;
}

LS_CASE(am_recovers_the_modulation_tone)
{
    LS_EQ_INT(8192, receive(0.5f));
    double real = 0, imag = 0, energy = 0;
    for (int i = 4096; i < 8192; ++i) {
        real += audio[i] * cos(6.28318530718 * 1000 * i / 16000);
        imag += audio[i] * sin(6.28318530718 * 1000 * i / 16000);
        energy += (double)audio[i] * audio[i];
    }
    LS_CHECK(energy > 1000000);
    LS_CHECK(2 * (real * real + imag * imag) / (4096 * energy) > 0.90);
}

LS_CASE(am_rejects_an_unmodulated_carrier)
{
    LS_EQ_INT(8192, receive(0));
    double energy = 0;
    for (int i = 4096; i < 8192; ++i) energy += (double)audio[i] * audio[i];
    LS_CHECK(sqrt(energy / 4096) < 200);
}

LS_CASE(am_respects_output_capacity)
{
    fm_dsp_init(&dsp);
    memset(iq, 160, sizeof(iq));
    int16_t out[9];
    for (int i = 0; i < 9; ++i) out[i] = 1234;
    LS_EQ_INT(7, fm_demod_am(&dsp, iq, sizeof(iq), out, 7));
    LS_EQ_INT(1234, out[7]);
    LS_EQ_INT(1234, out[8]);
}
