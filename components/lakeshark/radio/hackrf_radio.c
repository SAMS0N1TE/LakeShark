/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "hackrf_radio.h"

const ls_radio_range_t ls_hackrf_frequency_ranges[1] = {
    {UINT64_C(1000000), UINT64_C(6000000000)},
};

const ls_radio_range_t ls_hackrf_sample_rate_ranges[1] = {
    {UINT64_C(2000000), UINT64_C(20000000)},
};

static const uint32_t s_filter_bandwidths[] = {
    1750000, 2500000, 3500000, 5000000, 5500000, 6000000,
    7000000, 8000000, 9000000, 10000000, 12000000, 14000000,
    15000000, 20000000, 24000000, 28000000,
};

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

void ls_hackrf_iq_s8_to_u8(uint8_t *samples, size_t bytes)
{
    if (!samples) return;
    /* LS-340: HackRF bulk samples are signed int8_t while every LakeShark DSP
     * path consumes RTL-style offset binary. Toggling the sign bit maps
     * -128/0/127 exactly to 0/128/255 without widening the hot stream. */
    for (size_t i = 0; i < bytes; ++i)
        samples[i] ^= UINT8_C(0x80);
}

void ls_hackrf_encode_frequency(uint64_t frequency_hz, uint8_t out[8])
{
    uint32_t mhz = (uint32_t)(frequency_hz / UINT64_C(1000000));
    uint32_t remainder_hz =
        (uint32_t)(frequency_hz % UINT64_C(1000000));
    put_le32(out, mhz);
    put_le32(out + 4, remainder_hz);
}

void ls_hackrf_encode_sample_rate(uint32_t sample_rate_hz, uint8_t out[8])
{
    put_le32(out, sample_rate_hz);
    put_le32(out + 4, 1);
}

uint32_t ls_hackrf_filter_bandwidth(uint32_t requested_hz,
                                    uint32_t sample_rate_hz)
{
    uint32_t target = requested_hz;
    if (target == 0)
        target = (uint32_t)(((uint64_t)sample_rate_hz * 3) / 4);
    uint32_t selected = s_filter_bandwidths[0];
    for (size_t i = 1;
         i < sizeof(s_filter_bandwidths) / sizeof(s_filter_bandwidths[0]);
         ++i) {
        if (s_filter_bandwidths[i] > target) break;
        selected = s_filter_bandwidths[i];
    }
    return selected;
}
