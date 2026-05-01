/*
 * vhf_demod.h - placeholder for FM voice demodulation.
 *
 * Currently a stub. When implemented, this will provide:
 *   - quadrature FM discriminator
 *   - decimation from sample-rate down to 16 kHz audio
 *   - de-emphasis filter (75us US / 50us EU)
 *   - squelch (RSSI or noise gate)
 *   - audio dispatch into audio_out
 */
#ifndef VHF_DEMOD_H
#define VHF_DEMOD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void vhf_demod_init(void);
void vhf_on_sample(uint8_t *iq, int len);

#ifdef __cplusplus
}
#endif

#endif
