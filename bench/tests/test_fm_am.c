#include "ls_test.h"
#include "fm_dsp.h"
#include <math.h>
#include <string.h>

static fm_dsp_t dsp;
static uint8_t iq[4096];
static int16_t audio[8192];

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
