#include "ls_test.h"
#include "fm_dsp.h"
#include <math.h>
#include <string.h>

static fm_dsp_t dsp;
static uint8_t iq[4096];
static int16_t audio[8192];

LS_CASE(nfm_squelch_rejects_retune_and_short_spikes)
{
    fm_dsp_init(&dsp);
    dsp.iq_block_peak = 1.0f;
    LS_CHECK(!fm_nfm_squelch(&dsp, 15, 1024));
    LS_CHECK(!fm_nfm_squelch(&dsp, 15, 1024));
    LS_CHECK(!fm_nfm_squelch(&dsp, 15, 1024));
    dsp.iq_block_peak = 0.08f;
    LS_CHECK(!fm_nfm_squelch(&dsp, 15, 1024));
    dsp.iq_block_peak = 0.30f;
    LS_CHECK(!fm_nfm_squelch(&dsp, 15, 1024));
    LS_CHECK(fm_nfm_squelch(&dsp, 15, 1024));
    dsp.iq_block_peak = 0.08f;
    LS_CHECK(!fm_nfm_squelch(&dsp, 15, 1024));
    fm_dsp_init(&dsp);
    dsp.iq_block_peak = 0.30f;
    LS_CHECK(!fm_nfm_squelch(&dsp, 15, 1024));
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
