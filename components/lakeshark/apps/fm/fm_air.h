/* The airband / CB scanner in FM's AM mode, on an SDR: the receive task's
   side of air_scan.c. It measures each block's spectrum, retunes through the
   hook app_fm.c installs, and when the scanner parks on a channel turns the
   block back down by a quarter of the sample rate and demodulates it.
   Requests from the screen arrive through fm_bcast.h. Receive task only. */
#ifndef FM_AIR_H
#define FM_AIR_H

#include <stdbool.h>
#include <stdint.h>

#include "fm_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How the scanner moves the LO, and where it goes back to when it stops. */
void fm_air_install(void (*tune)(uint32_t hz));
/* Take the screen's requests; may start or stop the scanner. Every block in
   AM mode. */
void fm_air_poll(uint32_t home_hz);
bool fm_air_running(void);
/* One block while running: returns 16 kHz audio samples to play, 0 when
   the scanner is between channels or the channel is quiet. */
int fm_air_block(fm_dsp_t *dsp, const uint8_t *iq, int iq_len, int16_t *pcm, int max);
/* The channel being listened to, 0 while scanning. */
uint32_t fm_air_listen_hz(void);
/* Leaving AM or a manual tune: stop, and say so to the screen. go_home
   puts the receiver back where it was before the scan started. */
void fm_air_stop(bool go_home);
/* For the stats line. */
const char *fm_air_summary(char *out, int n);
/* The scanner's measurement of one block: SPEC_FFT_N bins of dB,
   fftshifted, eight frames averaged. NULL without PSRAM. The weather
   channel finder uses it too. */
const float *fm_air_measure(const uint8_t *iq, int iq_len);

/* USB/LSB for the CB presets: hf_demod (the HF chain's demodulator) on the
   rotated block (channel at 0 Hz, FM_RTL_RATE), with the HF options' filter,
   AGC, blanker and clarifier. Returns the 16 kHz samples written, or -1 for
   a demod it does not do or no PSRAM for its state, and the preset then
   plays through AM. Sets dsp->iq_peak for the meter. */
int fm_air_ssb(int demod, fm_dsp_t *dsp, const uint8_t *iq, int iq_len,
               int16_t *pcm, int max);

#ifdef __cplusplus
}
#endif

#endif
