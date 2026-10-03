/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "hackrf_radio.h"
#include <math.h>
#include <string.h>

static uint32_t get_le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }

bool ls_hackrf_decode_m0_state(const uint8_t *wire, size_t bytes,
                               ls_radio_iq_health_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!wire || bytes != 40) return false;
    out->requested_mode = (uint16_t)get_le32(wire);
    out->request_flag = (uint16_t)(get_le32(wire) >> 16);
    out->active_mode = get_le32(wire+4);
    out->m0_count = get_le32(wire+8);
    out->m4_count = get_le32(wire+12);
    out->num_shortfalls = get_le32(wire+16);
    out->longest_shortfall = get_le32(wire+20);
    out->shortfall_limit = get_le32(wire+24);
    out->threshold = get_le32(wire+28);
    out->next_mode = get_le32(wire+32);
    out->error = get_le32(wire+36);
    return true;
}

const ls_radio_range_t ls_hackrf_frequency_ranges[1] = {
    {UINT64_C(1000000), UINT64_C(6000000000)},
};

const ls_radio_range_t ls_hackrf_sample_rate_ranges[1] = {
    /* Below 2 MSPS by decimation; see ls_hackrf_decimation. */
    {UINT64_C(100000), UINT64_C(20000000)},
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
    /* HackRF bulk samples are signed int8_t while every LakeShark DSP
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

unsigned ls_hackrf_decimation(uint32_t rate_hz)
{
    if (rate_hz == 0 || rate_hz > LS_HACKRF_HW_MAX_RATE) return 0;
    if (rate_hz >= LS_HACKRF_HW_MIN_RATE) return 1;
    const unsigned factor =
        (unsigned)((LS_HACKRF_HW_MIN_RATE + rate_hz - 1) / rate_hz);
    return factor <= LS_HACKRF_DECIM_MAX ? factor : 0;
}

unsigned ls_hackrf_shift(unsigned factor)
{
    /* Two output-rates when the radio's band has room for it, else one. */
    return factor >= 5 ? 2 : factor >= 3 ? 1 : 0;
}

void ls_hackrf_cic_init(ls_hackrf_cic_t *c, unsigned factor, unsigned shift)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->factor = factor < 2 ? 2 : factor > LS_HACKRF_DECIM_MAX ? LS_HACKRF_DECIM_MAX : factor;
    /* The wanted signal sits `shift` output-rates below centre; turning by
       shift whole cycles every output period brings it to zero. */
    for (unsigned j = 0; j < c->factor; ++j) {
        const double a = 2.0 * 3.14159265358979323846 * shift * j / c->factor;
        c->cos_q8[j] = (int16_t)lround(256.0 * cos(a));
        c->sin_q8[j] = (int16_t)lround(256.0 * sin(a));
    }
    /* The CIC gains factor^3; take that out, then put factor/2 back. The
       filter drops the noise outside the narrower band, so without it a
       quiet signal would sit a step or two above zero in eight bits. */
    const int64_t gain = (int64_t)c->factor * c->factor * c->factor * 256;
    c->inv = ((int64_t)c->factor << 31) / gain;
    c->agc_q8 = 256;
}

static uint8_t cic_out(ls_hackrf_cic_t *c, int rail)
{
    uint32_t y = c->integ[rail][2];
    for (int k = 0; k < 3; ++k) {
        const uint32_t d = y - c->comb[rail][k];
        c->comb[rail][k] = y;
        y = d;
    }
    /* Bounded by 182 * 256 * factor^3, so back to a signed value is exact. */
    int64_t scaled = ((int64_t)(int32_t)y * c->inv + ((int64_t)1 << 31)) >> 32;
    if (c->agc) {
        scaled = (scaled * c->agc_q8) >> 8;
        const int32_t mag = (int32_t)(scaled < 0 ? -scaled : scaled);
        c->agc_env -= c->agc_env >> 10;
        if (mag > c->agc_env) c->agc_env = mag;
    }
    const int64_t v = scaled < -128 ? -128 : scaled > 127 ? 127 : scaled;
    return (uint8_t)(v + 128);
}

#define AGC_MAX_Q8 (256 * 64)

static void agc_step(ls_hackrf_cic_t *c)
{
    if (++c->agc_count < 256) return;
    c->agc_count = 0;
    if (c->agc_env > 96) c->agc_q8 = c->agc_q8 * 7 / 8;
    else if (c->agc_env < 32) c->agc_q8 = c->agc_q8 * 9 / 8 + 1;
    if (c->agc_q8 < 32) c->agc_q8 = 32;
    if (c->agc_q8 > AGC_MAX_Q8) c->agc_q8 = AGC_MAX_Q8;
}

/* One I/Q pair into the integrators, with its output if the period ends. */
static size_t cic_pair(ls_hackrf_cic_t *c, int8_t iv, int8_t qv, uint8_t *out)
{
    const int32_t co = c->cos_q8[c->phase], si = c->sin_q8[c->phase];
    const int32_t mi = iv * co - qv * si, mq = iv * si + qv * co;
    uint32_t *a = c->integ[0], *b = c->integ[1];
    a[0] += (uint32_t)mi; a[1] += a[0]; a[2] += a[1];
    b[0] += (uint32_t)mq; b[1] += b[0]; b[2] += b[1];
    if (++c->phase < c->factor) return 0;
    c->phase = 0;
    out[0] = cic_out(c, 0);
    out[1] = cic_out(c, 1);
    if (c->agc) agc_step(c);
    return 2;
}

size_t ls_hackrf_cic_run(ls_hackrf_cic_t *c, const uint8_t *in, size_t bytes,
                         uint8_t *out)
{
    if (!c || !in || !out || c->factor < 2) return 0;
    size_t written = 0, i = 0;
    if (c->has_held && bytes) {
        c->has_held = false;
        written += cic_pair(c, c->held, (int8_t)in[i++], out);
    }
    /* The hot loop keeps the integrators and the phase in locals: through
       the struct, with `out` allowed to alias `in`, every sample reloaded
       and stored all six. They go back to the struct only for an output. */
    const unsigned factor = c->factor;
    const int16_t *const cs = c->cos_q8, *const sn = c->sin_q8;
    uint32_t a0 = c->integ[0][0], a1 = c->integ[0][1], a2 = c->integ[0][2];
    uint32_t b0 = c->integ[1][0], b1 = c->integ[1][1], b2 = c->integ[1][2];
    unsigned phase = c->phase;
    for (; i + 1 < bytes; i += 2) {
        const int32_t iv = (int8_t)in[i], qv = (int8_t)in[i + 1];
        const int32_t co = cs[phase], si = sn[phase];
        a0 += (uint32_t)(iv * co - qv * si); a1 += a0; a2 += a1;
        b0 += (uint32_t)(iv * si + qv * co); b1 += b0; b2 += b1;
        if (++phase == factor) {
            phase = 0;
            c->integ[0][2] = a2;
            c->integ[1][2] = b2;
            out[written++] = cic_out(c, 0);
            out[written++] = cic_out(c, 1);
            if (c->agc) agc_step(c);
        }
    }
    c->integ[0][0] = a0; c->integ[0][1] = a1; c->integ[0][2] = a2;
    c->integ[1][0] = b0; c->integ[1][1] = b1; c->integ[1][2] = b2;
    c->phase = phase;
    if (i < bytes) { c->held = (int8_t)in[i]; c->has_held = true; }
    return written;
}
