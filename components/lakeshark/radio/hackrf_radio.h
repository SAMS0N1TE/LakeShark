/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_HACKRF_RADIO_H
#define LS_HACKRF_RADIO_H

#include <stddef.h>
#include <stdint.h>

#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_HACKRF_CAPABILITIES LS_RADIO_RX_IQ_U8
#define LS_HACKRF_DUPLEX LS_RADIO_DUPLEX_HALF
#define LS_HACKRF_MIN_STREAM_BYTES_PER_SEC UINT32_C(4000000)

extern const ls_radio_range_t ls_hackrf_frequency_ranges[1];
extern const ls_radio_range_t ls_hackrf_sample_rate_ranges[1];

void ls_hackrf_iq_s8_to_u8(uint8_t *samples, size_t bytes);
void ls_hackrf_encode_frequency(uint64_t frequency_hz, uint8_t out[8]);
void ls_hackrf_encode_sample_rate(uint32_t sample_rate_hz, uint8_t out[8]);
uint32_t ls_hackrf_filter_bandwidth(uint32_t requested_hz,
                                    uint32_t sample_rate_hz);

#ifdef __cplusplus
}
#endif

#endif
