#include "ls_test.h"
#include "fm_lr_dsp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* The narrow LR path's DC blocker: the chip's LO leakage sits at 0 Hz in
   the 125k channel. Here it is a fixed offset stronger than the station. */
#define PI 3.14159265358979323846
static fm_lr_dsp_t lr;
static fm_dsp_t fm;
static void pack(uint8_t *p, int i, int q)
{
    p[0] = (uint8_t)i; p[1] = (uint8_t)((unsigned)i >> 8);
    p[2] = (uint8_t)q; p[3] = (uint8_t)((unsigned)q >> 8);
}
/* A 1 kHz tone at 3 kHz deviation on a carrier 2.5 kHz off the LO, plus
   the leakage; returns the audio tone's SNR in dB. */
static double tone_snr(bool block, int dc_i, int dc_q)
{
    fm_lr_init(&lr, 25000); fm_dsp_init(&fm);
    fm_lr_set_dc_block(&lr, block);
    double phase = 0, ss = 0, cc = 0, power = 0, sum = 0;
    int used = 0, total = 0, samples = 0;
    for (int b = 0; b < 800; ++b) {
        uint8_t iq[1280]; float demod[84]; int16_t pcm[44];
        for (int n = 0; n < 320; ++n, ++samples) {
            phase += 2 * PI * (2500 + 3000 * sin(2 * PI * 1000 * samples / 125000.0)) / 125000;
            pack(iq + 4 * n, (int)lrint(6000 * cos(phase)) + dc_i, (int)lrint(6000 * sin(phase)) + dc_q);
        }
        const int nd = fm_lr_demod(&lr, &fm, iq, sizeof(iq), demod, 84);
        const int n = fm_demod_to_audio(&fm, demod, nd, pcm, 44);
        for (int k = 0; k < n; ++k, ++total) if (total >= 8000 && used < 16000) {
            const double a = 2 * PI * 1000 * used / 16000;
            ss += pcm[k] * sin(a); cc += pcm[k] * cos(a); power += (double)pcm[k] * pcm[k];
            sum += pcm[k]; ++used;
        }
    }
    /* The carrier's 2.5 kHz offset is a steady level in the audio: not noise. */
    const double signal = 2 * (ss * ss + cc * cc) / used;
    return 10 * log10(signal / (power - sum * sum / used - signal));
}

LS_CASE(blocker_restores_a_station_under_lo_leakage)
{
    const double clean = tone_snr(false, 0, 0);
    const double leaky = tone_snr(false, 9000, -7000);
    const double fixed = tone_snr(true, 9000, -7000);
    printf("tone SNR: clean %.1f dB, leakage %.1f dB, leakage + blocker %.1f dB\n", clean, leaky, fixed);
    LS_CHECK(fixed > 20);
    LS_CHECK(fixed > leaky + 10);
    /* And costs a clean station nothing it can hear: the mean's ripple from
       the nearest sideband (500 Hz here) stays some 40 dB down. */
    const double kept = tone_snr(true, 0, 0);
    printf("clean + blocker %.1f dB\n", kept);
    LS_CHECK(kept > 40);
}

LS_CASE(blocker_settles_pure_dc_to_nothing)
{
    fm_lr_init(&lr, 12500); fm_dsp_init(&fm); fm_lr_set_dc_block(&lr, true);
    uint8_t iq[1280]; float demod[84];
    for (int n = 0; n < 320; ++n) pack(iq + 4 * n, 12000, -12000);
    for (int b = 0; b < 400; ++b) fm_lr_demod(&lr, &fm, iq, sizeof(iq), demod, 84);  /* ~1 s */
    for (int k = 0; k < FM_LR_TAPS; ++k)
        LS_CHECK_MSG(fabsf(lr.i[k]) < 1e-3f && fabsf(lr.q[k]) < 1e-3f, "tap %d: %g %g", k, lr.i[k], lr.q[k]);
}

LS_CASE(reset_keeps_the_setting_and_clears_the_mean)
{
    fm_lr_init(&lr, 25000); fm_lr_set_dc_block(&lr, true);
    uint8_t iq[1280]; float demod[84];
    for (int n = 0; n < 320; ++n) pack(iq + 4 * n, 5000, 5000);
    fm_dsp_init(&fm); fm_lr_demod(&lr, &fm, iq, sizeof(iq), demod, 84);
    LS_CHECK(lr.dc_i != 0);
    fm_lr_reset(&lr);
    LS_CHECK(lr.dc_block); LS_CHECK(lr.dc_i == 0 && lr.dc_q == 0);
    /* Off again is the old path exactly: the same output as never on. */
    fm_lr_dsp_t never; fm_dsp_t fm2;
    fm_lr_init(&never, 25000); fm_lr_set_dc_block(&lr, false);
    fm_lr_reset(&lr); fm_dsp_init(&fm); fm_dsp_init(&fm2);
    float a[84], b[84];
    const int na = fm_lr_demod(&lr, &fm, iq, sizeof(iq), a, 84);
    const int nb = fm_lr_demod(&never, &fm2, iq, sizeof(iq), b, 84);
    LS_EQ_INT(nb, na); LS_CHECK(!memcmp(a, b, sizeof(float) * (size_t)na));
}
