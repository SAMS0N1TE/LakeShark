/* LR433 decoders: ported from rtl_433 (https://github.com/merbanan/rtl_433),
   src/bitbuffer.c, src/pulse_slicer.c, src/bit_util.c and the device files
   named beside each decoder below, all GPL-2.0-or-later. The upstream
   notices:

     bitbuffer.c, pulse_slicer.c, bit_util.c
       Copyright (C) 2015 Tommy Vestermark
     schraeder.c
       Copyright (C) 2016 Benjamin Larsson
       and 2017 Christian W. Zuckschwerdt <zany@triq.net>
     tpms_gm.c          Copyright (C) 2025 Eric Blevins
     tpms_ford.c, tpms_toyota.c, tpms_pmv107j.c, tpms_citroen.c
       Copyright (C) 2017 Christian W. Zuckschwerdt <zany@triq.net>
     tpms_hyundai_vdo.c
       Copyright (C) 2020 Todor Uzunov aka teou, TTiges, 2019 Andreas Spiess,
       2017 Christian W. Zuckschwerdt <zany@triq.net>
     tpms_kia.c
       Copyright (C) 2022 Lasse Mikkel Reinhold, Todor Uzunov aka teou,
       TTiges, 2019 Andreas Spiess, 2017 Christian W. Zuckschwerdt
     tpms_elantra2012.c Copyright (C) 2019 Kumar Vivek <kv2000in@gmail.com>
     acurite.c
       Copyright (c) 2015, Jens Jenson, Helge Weissig, David Ray Thompson,
       Robert Terzi
     lacrosse_tx141x.c  Copyright (C) 2017 Robert Fraczkiewicz <aromring@gmail.com>
     ambient_weather.c  contributed by David Ediger, discovered by Ron C. Lewis
     fineoffset.c
       Copyright (C) 2017 Tommy Vestermark
       Enhanced (C) 2019 Christian W. Zuckschwerdt <zany@triq.net>
     ambientweather_wh31e.c
       Copyright (C) 2018 Christian W. Zuckschwerdt <zany@triq.net>
     ert_scm.c          Copyright (C) 2020 Benjamin Larsson.
     scmplus.c, ert_idm.c
       Copyright (C) 2020 Peter Shipley <peter.shipley@gmail.com>

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published by
     the Free Software Foundation; either version 2 of the License, or
     (at your option) any later version.

   Differences from upstream, all forced by the input: widths are counted in
   the chip's decisions rather than SDR samples, so the slicers round a
   width in microseconds to the nearest decision instead of truncating it; a
   capture is a fixed number of decisions, so decoders that judged a message
   by the exact length of its row take a row at least that long and read
   from where the message starts; and FSK polarity is not known, so the FSK
   decoders run on both. Itron's rtlamr (AGPL-3.0) is not used: the ERT
   framing below comes from rtl_433's ERT decoders. */

#include "lr433_dec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ bit tools -- */

typedef lr433_bitbuf_t bitbuf_t;

/* One row of decoded bits, small enough for the stack. */
typedef struct {
    uint16_t bits;
    uint8_t b[64];
} row_t;

static void row_add(row_t *r, int bit)
{
    if (r->bits >= sizeof(r->b) * 8) return;
    if (bit) r->b[r->bits >> 3] |= (uint8_t)(0x80 >> (r->bits & 7));
    r->bits++;
}

/* bitbuffer_invert on one row: the bits past its length stay zero. */
static void row_invert(row_t *r)
{
    if (!r->bits) return;
    const unsigned last_col = (r->bits - 1u) / 8, last_bits = ((r->bits - 1u) % 8) + 1;
    for (unsigned i = 0; i <= last_col; i++) r->b[i] = (uint8_t)~r->b[i];
    r->b[last_col] ^= (uint8_t)(0xFF >> last_bits);
}

static inline unsigned bit_at(const uint8_t *bytes, unsigned bit)
{
    return (bytes[bit >> 3] >> (7 - (bit & 7))) & 1u;
}

static void bb_clear(bitbuf_t *b) { memset(b, 0, sizeof(*b)); }

static void bb_add_bit(bitbuf_t *b, int bit)
{
    if (b->num_rows == 0) b->free_row = b->num_rows = 1;
    const unsigned r = b->num_rows - 1u;
    const unsigned n = b->bits_per_row[r];
    if (n >= LR433_BB_COLS * 8) return;       /* a row this long is noise */
    if (bit) b->bb[r][n >> 3] |= (uint8_t)(0x80 >> (n & 7));
    b->bits_per_row[r]++;
}

static void bb_add_row(bitbuf_t *b)
{
    if (b->num_rows == 0) b->free_row = b->num_rows = 1;
    if (b->free_row < LR433_BB_ROWS) {
        b->free_row++;
        b->num_rows = b->free_row;
    } else {
        b->bits_per_row[b->num_rows - 1] = 0;
        memset(b->bb[b->num_rows - 1], 0, LR433_BB_COLS);
    }
}

static void bb_add_sync(bitbuf_t *b)
{
    if (b->num_rows == 0) b->free_row = b->num_rows = 1;
    if (b->bits_per_row[b->num_rows - 1]) bb_add_row(b);
    b->syncs_before_row[b->num_rows - 1]++;
}

static void bb_invert(bitbuf_t *b)
{
    for (unsigned r = 0; r < b->num_rows; r++) {
        const unsigned n = b->bits_per_row[r];
        if (!n) continue;
        const unsigned last_col = (n - 1) / 8, last_bits = ((n - 1) % 8) + 1;
        for (unsigned c = 0; c <= last_col; c++) b->bb[r][c] = (uint8_t)~b->bb[r][c];
        b->bb[r][last_col] ^= (uint8_t)(0xFF >> last_bits);
    }
}

static void extract_bytes(const uint8_t *bits, unsigned pos, uint8_t *out, unsigned len)
{
    if (!len) return;
    const unsigned bytes = (len + 7) / 8;
    for (unsigned i = 0; i < bytes; i++) {
        uint8_t v = 0;
        for (unsigned k = 0; k < 8; k++) v = (uint8_t)((v << 1) | bit_at(bits, pos + i * 8 + k));
        out[i] = v;
    }
    if (len & 7) out[(len - 1) / 8] &= (uint8_t)(0xff00 >> (len & 7));
}

/* bitbuffer_search: the first position at or after `start` where the
   pattern's bits are, or `len` when nowhere. */
static unsigned search(const uint8_t *bits, unsigned len, unsigned start,
                       const uint8_t *pattern, unsigned pattern_bits)
{
    unsigned ipos = start, ppos = 0;
    while (ipos < len && ppos < pattern_bits) {
        if (bit_at(bits, ipos) == bit_at(pattern, ppos)) {
            ppos++;
            ipos++;
            if (ppos == pattern_bits) return ipos - pattern_bits;
        } else {
            ipos -= ppos;
            ipos++;
            ppos = 0;
        }
    }
    return len;
}

/* bitbuffer_manchester_decode: 01 is 0, 10 is 1, stop at the first pair
   that is neither. Returns the position it stopped at. */
static unsigned mc_decode(const uint8_t *bits, unsigned len, unsigned start, row_t *out, unsigned max)
{
    unsigned ipos = start;
    if (max && len > start + max * 2) len = start + max * 2;
    while (ipos + 1 < len) {
        const unsigned b1 = bit_at(bits, ipos), b2 = bit_at(bits, ipos + 1);
        ipos += 2;
        if (b1 == b2) break;
        row_add(out, (int)b2);
    }
    return ipos;
}

/* bitbuffer_differential_manchester_decode. */
static unsigned dmc_decode(const uint8_t *bits, unsigned len, unsigned start, row_t *out, unsigned max)
{
    unsigned ipos = start;
    unsigned bit1, bit2 = 0;
    if (max && len > start + max * 2) len = start + max * 2;
    /* The first long pulse sets the clock; a short one is skipped. */
    while (ipos + 2 < len) {
        bit1 = bit_at(bits, ipos++);
        bit2 = bit_at(bits, ipos++);
        const unsigned bit3 = bit_at(bits, ipos);
        if (bit1 != bit2) {
            if (bit2 != bit3) {
                row_add(out, 0);
            } else {
                bit2 = bit1;
                ipos -= 1;
                break;
            }
        } else {
            bit2 = 1 - bit1;
            ipos -= 2;
            break;
        }
    }
    while (ipos + 1 < len) {
        bit1 = bit_at(bits, ipos++);
        if (bit1 == bit2) break;            /* clock missing */
        bit2 = bit_at(bits, ipos++);
        row_add(out, bit1 == bit2 ? 1 : 0);
    }
    return ipos;
}

static int compare_rows(const bitbuf_t *b, unsigned ra, unsigned rb, unsigned max_bits)
{
    if (max_bits == 0 || b->bits_per_row[ra] < max_bits || b->bits_per_row[rb] < max_bits)
        return b->bits_per_row[ra] == b->bits_per_row[rb] &&
               !memcmp(b->bb[ra], b->bb[rb], (b->bits_per_row[ra] + 7u) / 8u);
    const unsigned last = (max_bits - 1) / 8;
    const unsigned mask = 0xff00u >> (max_bits & 7);
    return !memcmp(b->bb[ra], b->bb[rb], max_bits / 8) &&
           (b->bb[ra][last] & mask) == (b->bb[rb][last] & mask);
}

static int find_repeated_row(const bitbuf_t *b, unsigned min_repeats, unsigned min_bits)
{
    for (unsigned i = 0; i < b->num_rows; i++) {
        if (b->bits_per_row[i] < min_bits) continue;
        unsigned cnt = 0;
        for (unsigned j = 0; j < b->num_rows; j++)
            if (compare_rows(b, i, j, 0)) cnt++;
        if (cnt >= min_repeats) return (int)i;
    }
    return -1;
}

uint8_t lr433_crc8(const uint8_t *m, unsigned n, uint8_t poly, uint8_t init)
{
    uint8_t r = init;
    for (unsigned i = 0; i < n; i++) {
        r ^= m[i];
        for (int k = 0; k < 8; k++) r = (r & 0x80) ? (uint8_t)((r << 1) ^ poly) : (uint8_t)(r << 1);
    }
    return r;
}

uint16_t lr433_crc16(const uint8_t *m, unsigned n, uint16_t poly, uint16_t init)
{
    uint16_t r = init;
    for (unsigned i = 0; i < n; i++) {
        r ^= (uint16_t)(m[i] << 8);
        for (int k = 0; k < 8; k++) r = (r & 0x8000) ? (uint16_t)((r << 1) ^ poly) : (uint16_t)(r << 1);
    }
    return r;
}

static uint8_t lfsr_digest8(const uint8_t *m, unsigned bytes, uint8_t gen, uint8_t key)
{
    uint8_t sum = 0;
    for (unsigned k = 0; k < bytes; k++) {
        for (int i = 7; i >= 0; i--) {
            if ((m[k] >> i) & 1) sum ^= key;
            key = (key & 1) ? (uint8_t)((key >> 1) ^ gen) : (uint8_t)(key >> 1);
        }
    }
    return sum;
}

static uint8_t lfsr_digest8_reflect(const uint8_t *m, int bytes, uint8_t gen, uint8_t key)
{
    uint8_t sum = 0;
    for (int k = bytes - 1; k >= 0; k--) {
        for (int i = 0; i < 8; i++) {
            if ((m[k] >> i) & 1) sum ^= key;
            key = (key & 0x80) ? (uint8_t)((key << 1) ^ gen) : (uint8_t)(key << 1);
        }
    }
    return sum;
}

static int add_bytes(const uint8_t *m, unsigned n)
{
    int s = 0;
    for (unsigned i = 0; i < n; i++) s += m[i];
    return s;
}

static int parity_bytes(const uint8_t *m, unsigned n)
{
    int r = 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t b = m[i];
        b ^= b >> 4;
        b &= 0xf;
        r ^= (0x6996 >> b) & 1;
    }
    return r;
}

/* ----------------------------------------------------------- the pulses -- */

int lr433_pulses(const lr433_capture_t *cap, bool invert, lr433_pulses_t *p)
{
    p->rate = cap->rate;
    p->n = 0;
    const int total = cap->lead_bits + cap->bits;
    int i = 0;
    const unsigned flip = invert ? 1u : 0u;
#define LEVEL(k) ((((k) < cap->lead_bits)                                                  \
                       ? (unsigned)((cap->lead >> (cap->lead_bits - 1 - (k))) & 1u)        \
                       : bit_at(cap->data, (unsigned)((k) - cap->lead_bits))) ^ flip)
    while (i < total && !LEVEL(i)) i++;             /* from the first 1 */
    while (i < total && p->n < LR433_MAX_PULSES) {
        int hi = 0, lo = 0;
        while (i < total && LEVEL(i)) { hi++; i++; }
        while (i < total && !LEVEL(i)) { lo++; i++; }
        /* The capture ending is the end of the message. */
        if (i >= total) lo = 0xFFFF;
        p->pulse[p->n] = (uint16_t)(hi > 0xFFFF ? 0xFFFF : hi);
        p->gap[p->n] = (uint16_t)(lo > 0xFFFF ? 0xFFFF : lo);
        p->n++;
    }
#undef LEVEL
    return p->n;
}

/* --------------------------------------------------------- the decoders -- */

typedef enum { MOD_PCM, MOD_PWM, MOD_PPM, MOD_MC_ZEROBIT } mod_t;

typedef struct ctx ctx_t;

typedef struct {
    uint8_t mod;
    uint16_t short_us, long_us, sync_us, gap_us, reset_us, tol_us;
    int (*cb)(ctx_t *c, bitbuf_t *b);
} decoder_t;

struct ctx {
    lr433_msg_t *out;
    int max, n;
    lr433_msg_t spare;    /* written when `out` is full, never read */
};

static lr433_msg_t *msg_new(ctx_t *c, int proto, const char *model)
{
    lr433_msg_t *m = c->n < c->max ? &c->out[c->n] : &c->spare;
    memset(m, 0, sizeof(*m));
    m->proto = (uint8_t)proto;
    m->channel = -1;
    m->battery_ok = -1;
    snprintf(m->model, sizeof(m->model), "%s", model);
    m->kpa = NAN;
    m->temp_c = NAN;
    m->humidity = NAN;
    m->consumption = -1;
    return m;
}

static bool same_msg(const lr433_msg_t *a, const lr433_msg_t *b)
{
    return a->proto == b->proto && a->channel == b->channel && a->battery_ok == b->battery_ok &&
           !strcmp(a->model, b->model) && !strcmp(a->id, b->id) &&
           ((isnan(a->kpa) && isnan(b->kpa)) || a->kpa == b->kpa) &&
           ((isnan(a->temp_c) && isnan(b->temp_c)) || a->temp_c == b->temp_c) &&
           ((isnan(a->humidity) && isnan(b->humidity)) || a->humidity == b->humidity) &&
           a->consumption == b->consumption;
}

/* Keep the message msg_new handed out, unless an identical one is already
   kept. Returns 1 either way: it was heard. */
static int msg_done(ctx_t *c)
{
    if (c->n >= c->max) return 1;
    for (int i = 0; i < c->n; i++)
        if (same_msg(&c->out[i], &c->out[c->n])) return 1;
    c->n++;
    return 1;
}

#define PSI_KPA 6.894757f

/* schraeder.c: Schrader GG4, 68 bits. */
static int schrader_cb(ctx_t *c, bitbuf_t *bb)
{
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        if (bb->bits_per_row[r] < 68 || bb->bits_per_row[r] > 72) continue;
        uint8_t b[8];
        extract_bytes(bb->bb[r], 4, b, 64);
        if (b[7] != lr433_crc8(b, 7, 0x07, 0xf0)) continue;
        const int serial = (b[1] & 0x0F) << 24 | b[2] << 16 | b[3] << 8 | b[4];
        lr433_msg_t *m = msg_new(c, LR433_P_SCHRADER, "Schrader");
        snprintf(m->id, sizeof(m->id), "%07X", serial);
        m->kpa = b[5] * 25 * 0.1f;
        m->temp_c = (float)(b[6] - 50);
        found += msg_done(c);
    }
    return found;
}

/* schraeder.c: Schrader EG53MA4, 120 bits. */
static int schrader_eg53ma4_cb(ctx_t *c, bitbuf_t *bb)
{
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        if (bb->bits_per_row[r] < 120 || bb->bits_per_row[r] > 124) continue;
        uint8_t b[10];
        extract_bytes(bb->bb[r], 40, b, 80);
        if (!b[1] && !b[2] && !b[4] && !b[5] && !b[7] && !b[8]) continue;
        if ((add_bytes(b, 9) & 0xff) != b[9]) continue;
        lr433_msg_t *m = msg_new(c, LR433_P_SCHRADER_EG53MA4, "Schrader-EG53MA4");
        snprintf(m->id, sizeof(m->id), "%06X", (b[4] << 16) | (b[5] << 8) | b[6]);
        m->kpa = b[7] * 25 * 0.1f;
        m->temp_c = (b[8] - 32.0f) * 5.0f / 9.0f;
        found += msg_done(c);
    }
    return found;
}

/* schraeder.c: Schrader SMD3MA4, 36-bit preamble then 38 Manchester bits. */
static int schrader_smd3ma4_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[2] = { 0x55, 0x5e };
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned len = bb->bits_per_row[r];
        if (len < 36 / 2 + 2 * 38) continue;
        unsigned pos = search(bb->bb[r], len, 0, pre, 16);
        if (pos >= len) continue;
        pos += 14;
        if (pos + 38 * 2 > len) continue;
        row_t d = { 0 };
        if (mc_decode(bb->bb[r], len, pos, &d, 38) != pos + 38 * 2) continue;
        row_invert(&d);
        const uint8_t *b = d.b;
        if (!b[0] && !b[1] && !b[2] && !b[3]) continue;
        int sum = 0;
        for (int i = 0; i < 5; i++)
            sum += ((b[i] >> 0) & 3) + ((b[i] >> 2) & 3) + ((b[i] >> 4) & 3) + ((b[i] >> 6) & 3);
        if ((sum & 3) != 1) continue;
        lr433_msg_t *m = msg_new(c, LR433_P_SCHRADER_SMD3MA4, "Schrader-SMD3MA4");
        const int serial = ((b[0] & 0x0f) << 20) | (b[1] << 12) | (b[2] << 4) | (b[3] >> 4);
        snprintf(m->id, sizeof(m->id), "%06X", serial);
        m->kpa = (((b[3] & 0x0f) << 4) | (b[4] >> 4)) * 0.2f * PSI_KPA;
        found += msg_done(c);
    }
    return found;
}

/* tpms_gm.c: GM aftermarket, 130 bits with a 48-bit zero preamble. */
static int gm_cb(ctx_t *c, bitbuf_t *bb)
{
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        if (bb->bits_per_row[r] < 130 || bb->bits_per_row[r] > 134) continue;
        uint8_t b[17] = { 0 };
        extract_bytes(bb->bb[r], 0, b, 130);
        static const uint8_t zero[6] = { 0 };
        if (memcmp(b, zero, 6)) continue;
        uint8_t sum = 0;
        int all_zero = 1;
        for (int i = 6; i < 15; i++) {
            sum = (uint8_t)(sum + b[i]);
            all_zero &= b[i] == 0;
        }
        if (sum != b[15] || (all_zero && b[15] == 0)) continue;
        const uint64_t id = ((uint64_t)b[8] << 32) | ((uint64_t)b[9] << 24) | ((uint32_t)b[10] << 16) |
                            ((uint32_t)b[11] << 8) | b[12];
        const int flags = (b[6] << 8) | b[7];
        lr433_msg_t *m = msg_new(c, LR433_P_GM_AFTERMARKET, "GM-Aftermarket");
        snprintf(m->id, sizeof(m->id), "%llu", (unsigned long long)id);
        m->kpa = b[13] * 2.75f;
        m->temp_c = (float)(b[14] - 60);
        m->battery_ok = !((flags >> 5) & 1);
        found += msg_done(c);
    }
    return found;
}

/* Every preamble match in every row, the FSK way: decode at each. */
typedef int (*at_fn)(ctx_t *c, const uint8_t *bits, unsigned len, unsigned pos);

static int each_match(ctx_t *c, bitbuf_t *bb, const uint8_t *pre, unsigned pre_bits,
                      unsigned need, unsigned step, unsigned skip, at_fn fn)
{
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned len = bb->bits_per_row[r];
        unsigned pos = 0;
        while ((pos = search(bb->bb[r], len, pos, pre, pre_bits)) + need <= len) {
            found += fn(c, bb->bb[r], len, pos + skip);
            pos += step;
        }
    }
    return found;
}

/* tpms_ford.c */
static int ford_at(ctx_t *c, const uint8_t *bits, unsigned len, unsigned pos)
{
    row_t p = { 0 };
    mc_decode(bits, len, pos, &p, 160);
    if (p.bits < 64) return 0;
    const uint8_t *b = p.b;
    if (((b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6]) & 0xff) != b[7]) return 0;
    int unknown = 0;
    switch (b[6] & 0x4c) {
    case 0x8: case 0x4: case 0x44: break;
    default: unknown = b[6] & 0x4c; break;
    }
    unknown |= b[6] & 0x90;
    if (unknown) return 0;
    lr433_msg_t *m = msg_new(c, LR433_P_FORD, "Ford");
    snprintf(m->id, sizeof(m->id), "%08x", (unsigned)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
    m->kpa = ((((b[6] & 0x20) << 3) | b[4]) * 0.25f) * PSI_KPA;
    if ((b[5] & 0x80) == 0) m->temp_c = (float)((b[5] & 0x7f) - 56);
    return msg_done(c);
}

static int ford_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[2] = { 0xaa, 0xa9 };
    bb_invert(bb);
    return each_match(c, bb, pre, 16, 144, 15, 16, ford_at);
}

/* tpms_toyota.c: PMV-C210. */
static int toyota_at(ctx_t *c, const uint8_t *bits, unsigned len, unsigned pos)
{
    row_t p = { 0 };
    const unsigned end = dmc_decode(bits, len, pos, &p, 80);
    if (end - pos < 144) return 0;
    const uint8_t *b = p.b;
    if (lr433_crc8(b, 8, 0x07, 0x80) != b[8]) return 0;
    const unsigned pressure1 = (b[4] & 0x7f) << 1 | b[5] >> 7;
    const unsigned temp = (b[5] & 0x7f) << 1 | b[6] >> 7;
    const unsigned pressure2 = b[7] ^ 0xff;
    if (pressure1 != pressure2) return 0;
    lr433_msg_t *m = msg_new(c, LR433_P_TOYOTA, "Toyota");
    snprintf(m->id, sizeof(m->id), "%08x", (unsigned)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
    m->kpa = (pressure1 * 0.25f - 7.0f) * PSI_KPA;
    m->temp_c = (float)temp - 40.0f;
    return msg_done(c);
}

static int toyota_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[2] = { 0xa9, 0xe0 };
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned len = bb->bits_per_row[r];
        unsigned pos = 0;
        while ((pos = search(bb->bb[r], len, pos, pre, 12)) + 156 <= len) {
            found += toyota_at(c, bb->bb[r], len, pos + 11);
            pos += 2;
        }
    }
    return found;
}

/* tpms_pmv107j.c */
static int pmv107j_at(ctx_t *c, const uint8_t *bits, unsigned len, unsigned pos)
{
    row_t p = { 0 };
    const unsigned end = dmc_decode(bits, len, pos, &p, 70);
    if (end - pos < 67 * 2) return 0;
    uint8_t b[9];
    b[0] = p.b[0] >> 6;
    extract_bytes(p.b, 2, b + 1, 64);
    if (lr433_crc8(b, 8, 0x13, 0x00) != b[8]) return 0;
    const unsigned pressure1 = b[5], pressure2 = b[6] ^ 0xff;
    if (pressure1 != pressure2) return 0;
    const unsigned id = (unsigned)b[0] << 26 | b[1] << 18 | b[2] << 10 | b[3] << 2 | b[4] >> 6;
    lr433_msg_t *m = msg_new(c, LR433_P_PMV107J, "PMV-107J");
    snprintf(m->id, sizeof(m->id), "%08x", id);
    m->battery_ok = !((b[4] & 0x20) >> 5);
    m->kpa = (pressure1 - 40.0f) * 2.48f;
    m->temp_c = b[7] - 40.0f;
    return msg_done(c);
}

static int pmv107j_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[1] = { 0xf8 };
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned len = bb->bits_per_row[r];
        unsigned pos = 0;
        while ((pos = search(bb->bb[r], len, pos, pre, 6)) + 67 * 2 <= len) {
            found += pmv107j_at(c, bb->bb[r], len, pos + 6);
            pos += 2;
        }
    }
    return found;
}

/* tpms_citroen.c */
static int citroen_at(ctx_t *c, const uint8_t *bits, unsigned len, unsigned pos)
{
    row_t p = { 0 };
    mc_decode(bits, len, pos, &p, 88);
    if (p.bits < 80) return 0;
    const uint8_t *b = p.b;
    if (b[6] == 0 || b[7] == 0) return 0;
    if ((b[1] ^ b[2] ^ b[3] ^ b[4] ^ b[5] ^ b[6] ^ b[7] ^ b[8] ^ b[9]) != 0) return 0;
    lr433_msg_t *m = msg_new(c, LR433_P_CITROEN, "Citroen");
    snprintf(m->id, sizeof(m->id), "%08x", (unsigned)b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4]);
    m->kpa = b[6] * 1.364f;
    m->temp_c = b[7] - 50.0f;
    return msg_done(c);
}

static int citroen_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[2] = { 0xaa, 0xa9 };
    bb_invert(bb);
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned len = bb->bits_per_row[r];
        unsigned pos = 0;
        while ((pos = search(bb->bb[r], len, pos, pre, 16)) + 178 <= len) {
            found += citroen_at(c, bb->bb[r], len, pos + 16);
            pos += 2;
        }
    }
    return found;
}

/* tpms_hyundai_vdo.c */
static int hyundai_vdo_at(ctx_t *c, const uint8_t *bits, unsigned len, unsigned pos)
{
    row_t p = { 0 };
    mc_decode(bits, len, pos, &p, 80);
    if (p.bits < 80) return 0;
    const uint8_t *b = p.b;
    if (lr433_crc8(b, 9, 0x07, 0xaa) != b[9]) return 0;
    lr433_msg_t *m = msg_new(c, LR433_P_HYUNDAI_VDO, "Hyundai-VDO");
    snprintf(m->id, sizeof(m->id), "%08x", (unsigned)b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4]);
    m->kpa = b[6] * 1.375f;
    m->temp_c = b[7] - 50.0f;
    return msg_done(c);
}

static int hyundai_vdo_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[4] = { 0xaa, 0xaa, 0xaa, 0xa9 };
    bb_invert(bb);
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned len = bb->bits_per_row[r];
        unsigned pos = 0;
        while ((pos = search(bb->bb[r], len, pos, pre, 32)) + 80 <= len) {
            found += hyundai_vdo_at(c, bb->bb[r], len, pos + 32);
            pos += 2;
        }
    }
    return found;
}

/* tpms_kia.c */
static int kia_at(ctx_t *c, const uint8_t *bits, unsigned len, unsigned pos)
{
    row_t p = { 0 };
    const unsigned end = mc_decode(bits, len, pos, &p, 154 - 16);
    if (end - pos < 154 - 16) return 0;
    const uint8_t *b = p.b;
    const uint8_t crc = b[8] & (uint8_t)~0x7;
    if (crc != lr433_crc8(b, 8, 0x07, 0x76)) return 0;
    const uint8_t pressure = (uint8_t)(b[0] << 4 | b[1] >> 4);
    const uint8_t temperature = (uint8_t)(b[1] << 4 | b[2] >> 4);
    const unsigned id = (unsigned)b[2] << 28 | b[3] << 20 | b[4] << 12 | b[5] << 4 | b[6] >> 4;
    lr433_msg_t *m = msg_new(c, LR433_P_KIA, "Kia");
    snprintf(m->id, sizeof(m->id), "%08x", id);
    m->kpa = pressure / 5.0f * PSI_KPA;
    m->temp_c = temperature - 50.0f;
    return msg_done(c);
}

static int kia_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[2] = { 0xed, 0x71 };
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned len = bb->bits_per_row[r];
        unsigned pos = 0;
        while ((pos = search(bb->bb[r], len, pos, pre, 16)) + 154 <= len) {
            found += kia_at(c, bb->bb[r], len, pos + 16);
            pos += 2;
        }
    }
    return found;
}

/* tpms_elantra2012.c */
static int elantra_at(ctx_t *c, const uint8_t *bits, unsigned len, unsigned pos)
{
    row_t p = { 0 };
    mc_decode(bits, len, pos, &p, 64);
    if (p.bits < 64) return 0;
    const uint8_t *b = p.b;
    if (lr433_crc8(b, 8, 0x07, 0x00)) return 0;
    lr433_msg_t *m = msg_new(c, LR433_P_ELANTRA2012, "Elantra2012");
    snprintf(m->id, sizeof(m->id), "%08x", (unsigned)b[2] << 24 | b[3] << 16 | b[4] << 8 | b[5]);
    m->kpa = (float)(b[0] + 60);
    m->temp_c = (float)(b[1] - 50);
    m->battery_ok = !((b[6] & 0x02) >> 1);
    return msg_done(c);
}

static int elantra_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[2] = { 0x71, 0x55 };
    return each_match(c, bb, pre, 16, 128, 15, 16, elantra_at);
}

/* acurite.c: the "TXR" family, of which the tower sensor and the 5-in-1. */
static const char *acurite_channel(uint8_t byte)
{
    static const char *const ch[] = { "C", "E", "B", "A" };
    return ch[(byte & 0xC0) >> 6];
}

static bool acurite_txr_check(const uint8_t *bb, unsigned browlen, unsigned explen)
{
    if (browlen < 6 || browlen < explen) return false;
    if ((add_bytes(bb, explen - 1) & 0xff) != bb[explen - 1]) return false;
    if (parity_bytes(&bb[2], explen - 3)) return false;
    return *acurite_channel(bb[0]) != 'E';
}

static int acurite_txr_cb(ctx_t *c, bitbuf_t *bbuf)
{
    int found = 0;
    bb_invert(bbuf);
    for (unsigned r = 0; r < bbuf->num_rows; r++) {
        const unsigned browlen = bbuf->bits_per_row[r] / 8;
        const uint8_t *bb = bbuf->bb[r];
        if (browlen < 6 || browlen > 10) continue;
        if (bb[0] == 0 && bb[1] == 0 && bb[2] == 0 && bb[browlen - 1] == 0) continue;
        const uint8_t type = bb[2] & 0x3f;
        const char ch = *acurite_channel(bb[0]);
        if (type == 0x04) {
            if (!acurite_txr_check(bb, browlen, 7)) continue;
            const int humidity = bb[3] & 0x7f;
            if (humidity > 100 && humidity != 127) continue;
            const int temp_raw = ((bb[4] & 0x7F) << 7) | (bb[5] & 0x7F);
            const float tempc = (temp_raw - 1000) * 0.1f;
            if (tempc < -40 || tempc > 70) continue;
            lr433_msg_t *m = msg_new(c, LR433_P_ACURITE_TOWER, "Acurite-Tower");
            snprintf(m->id, sizeof(m->id), "%d", ((bb[0] & 0x3f) << 8) | bb[1]);
            m->channel = (int8_t)ch;
            m->battery_ok = (bb[2] & 0x40) != 0;
            m->temp_c = tempc;
            if (humidity != 127) m->humidity = (float)humidity;
            found += msg_done(c);
        } else if (type == 0x31 || type == 0x38) {
            if (!acurite_txr_check(bb, browlen, 8)) continue;
            const int id = ((bb[0] & 0x0f) << 8) | bb[1];
            if (type == 0x38) {
                const int temp_raw = (bb[4] & 0x0F) << 7 | (bb[5] & 0x7F);
                const float tempf = (temp_raw - 400) * 0.1f;
                const int humidity = bb[6] & 0x7f;
                if (tempf < -40.0f || tempf > 158.0f || humidity > 100) continue;
                lr433_msg_t *m = msg_new(c, LR433_P_ACURITE_5N1, "Acurite-5n1");
                snprintf(m->id, sizeof(m->id), "%d", id);
                m->channel = (int8_t)ch;
                m->battery_ok = (bb[2] & 0x40) != 0;
                m->temp_c = (tempf - 32.0f) * 5.0f / 9.0f;
                m->humidity = (float)humidity;
                found += msg_done(c);
            } else {
                /* Wind and rain only: the table shows that it was heard. */
                lr433_msg_t *m = msg_new(c, LR433_P_ACURITE_5N1, "Acurite-5n1");
                snprintf(m->id, sizeof(m->id), "%d", id);
                m->channel = (int8_t)ch;
                m->battery_ok = (bb[2] & 0x40) != 0;
                found += msg_done(c);
            }
        }
    }
    return found;
}

/* lacrosse_tx141x.c */
static int lacrosse_tx141x_cb(ctx_t *c, bitbuf_t *bb)
{
    bb_invert(bb);
    int r = find_repeated_row(bb, bb->num_rows > 5 ? 5 : 3, 32);
    if (r < 0) r = find_repeated_row(bb, 2, 64);
    if (r < 0 && bb->num_rows <= 4) {
        for (unsigned row = 0; row < bb->num_rows; row++) {
            if ((bb->bits_per_row[row] == 40 || bb->bits_per_row[row] == 41) &&
                lfsr_digest8_reflect(bb->bb[row], 4, 0x31, 0xf4) == bb->bb[row][4]) {
                r = (int)row;
                break;
            }
        }
    }
    if (r < 0) return 0;
    const unsigned bits = bb->bits_per_row[r];
    const uint8_t *b = bb->bb[r];
    enum { TX141B, TX141, TX141TH, TX141BV3, TX141W } dev;
    if (bits >= 64) dev = TX141W;
    else if (bits > 41) return 0;
    else if (bits >= 41) {
        if (bb->num_rows > 12) return 0;
        dev = TX141TH;
    } else if (bits >= 40) dev = TX141TH;
    else if (bits >= 37) dev = TX141;
    else if (bits == 32) dev = TX141B;
    else dev = TX141BV3;

    if (dev == TX141W) {
        if ((b[0] >> 3) != 0x01) return 0;
        if (lr433_crc8(b, 8, 0x31, 0x00)) return 0;
        const int type = b[3] & 0x0f;
        if (type != 1 && type != 2) return 0;
        lr433_msg_t *m = msg_new(c, LR433_P_LACROSSE_TX141, "LaCrosse-TX141W");
        snprintf(m->id, sizeof(m->id), "%05x", ((b[0] & 0x07) << 16) | (b[1] << 8) | b[2]);
        m->channel = (int8_t)((b[3] & 0x30) >> 4);
        m->battery_ok = !(b[3] >> 7);
        if (type == 1) {
            m->temp_c = (((b[4] << 4) | (b[5] >> 4)) - 500) * 0.1f;
            m->humidity = (float)(((b[5] & 0x0f) << 8) | b[6]);
        }
        return msg_done(c);
    }
    const int id = b[0];
    const int battery_low = dev == TX141TH ? (b[1] >> 7) : !(b[1] >> 7);
    const int temp_raw = ((b[1] & 0x0F) << 8) | b[2];
    const float temp_c = (temp_raw - 500) * 0.1f;
    const int humidity = dev == TX141TH ? b[3] : 0;
    if (id == 0 || (dev == TX141TH && (humidity == 0 || humidity > 100)) || temp_c < -40.0f || temp_c > 140.0f)
        return 0;
    if (dev == TX141TH && lfsr_digest8_reflect(b, 4, 0x31, 0xf4) != b[4]) return 0;
    static const char *const names[] = { "LaCrosse-TX141B", "LaCrosse-TX141Bv2", "LaCrosse-TX141THBv2",
                                         "LaCrosse-TX141Bv3" };
    lr433_msg_t *m = msg_new(c, LR433_P_LACROSSE_TX141, names[dev]);
    snprintf(m->id, sizeof(m->id), "%02x", id);
    if (dev != TX141B) m->channel = (int8_t)((b[1] & 0x30) >> 4);
    m->battery_ok = !battery_low;
    m->temp_c = temp_c;
    if (dev == TX141TH) m->humidity = (float)humidity;
    return msg_done(c);
}

/* ambient_weather.c: F007TH and kin. */
static int f007th_at(ctx_t *c, const uint8_t *bits, unsigned len, unsigned pos)
{
    (void)len;
    uint8_t b[6];
    extract_bytes(bits, pos, b, 48);
    if (b[5] != (uint8_t)(lfsr_digest8(b, 5, 0x98, 0x3e) ^ 0x64)) return 0;
    const int temp_raw = ((b[2] & 0x0f) << 8) | b[3];
    const float temp_f = (temp_raw - 400) * 0.1f;
    if (b[4] > 100 || temp_f < -40.0f || temp_f >= 344.0f) return 0;
    lr433_msg_t *m = msg_new(c, LR433_P_AMBIENT_F007TH, "Ambientweather-F007TH");
    snprintf(m->id, sizeof(m->id), "%d", b[1]);
    m->channel = (int8_t)(((b[2] & 0x70) >> 4) + 1);
    m->battery_ok = (b[2] & 0x80) == 0;
    m->temp_c = (temp_f - 32.0f) * 5.0f / 9.0f;
    m->humidity = (float)b[4];
    return msg_done(c);
}

static int f007th_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[2] = { 0x01, 0x45 };
    static const uint8_t pre_inv[2] = { 0xfd, 0x45 };
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned len = bb->bits_per_row[r];
        unsigned pos = 0;
        while ((pos = search(bb->bb[r], len, pos, pre, 12)) + 8 + 6 * 8 <= len) {
            if (f007th_at(c, bb->bb[r], len, pos + 8)) return 1;
            pos += 16;
        }
        pos = 0;
        while ((pos = search(bb->bb[r], len, pos, pre_inv, 12)) + 8 + 6 * 8 <= len) {
            if (f007th_at(c, bb->bb[r], len, pos + 8)) return 1;
            pos += 15;
        }
    }
    return 0;
}

/* fineoffset.c: WH2, WH2A, WH5, Telldus. */
static int fineoffset_wh2_cb(ctx_t *c, bitbuf_t *bb)
{
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned bits = bb->bits_per_row[r];
        const uint8_t *row = bb->bb[r];
        uint8_t b[6] = { 0 };
        enum { WH2, WH2A, WH5, TP, TFA } model;
        if (bits == 48 && row[0] == 0xFF) {
            extract_bytes(row, 8, b, 40);
            model = WH2;
        } else if (bits == 55 && row[0] == 0xFE) {
            extract_bytes(row, 7, b, 48);
            model = b[3] == 0xff ? TFA : WH2A;
        } else if (bits == 47 && row[0] == 0xFE) {
            extract_bytes(row, 7, b, 40);
            model = WH5;
        } else if (bits == 49 && row[0] == 0xFF && (row[1] & 0x80) == 0x80) {
            extract_bytes(row, 9, b, 40);
            model = TP;
        } else {
            continue;
        }
        if (b[4] != lr433_crc8(b, 4, 0x31, 0)) continue;
        if (model == TFA && (add_bytes(b, 5) & 0xff) != b[5]) continue;
        if ((b[0] >> 4) != 4) continue;
        int temp = ((b[1] & 0x0F) << 8) | b[2];
        int low_battery = -1;
        if (model == TFA) {
            low_battery = (temp & 0x800) != 0;
            temp = (temp & 0x7FF) - 400;
        } else if (model == WH5) {
            temp -= 400;
        } else if (temp & 0x800) {
            temp = -(temp & 0x7FF);
        }
        const float temperature = temp * 0.1f;
        if (model == WH5 && (temperature < -40.0f || temperature > 60.0f)) continue;
        static const char *const names[] = { "Fineoffset-WH2", "Fineoffset-WH2A", "Fineoffset-WH5",
                                             "Fineoffset-TelldusProove", "TFA-303225" };
        lr433_msg_t *m = msg_new(c, LR433_P_FINEOFFSET_WH2, names[model]);
        snprintf(m->id, sizeof(m->id), "%d", ((b[0] & 0x0F) << 4) | ((b[1] & 0xF0) >> 4));
        if (low_battery >= 0) m->battery_ok = !low_battery;
        m->temp_c = temperature;
        if (b[3] != 0xff) m->humidity = (float)b[3];
        found += msg_done(c);
    }
    return found;
}

/* ambientweather_wh31e.c: WH31E and WH31B. */
static int wh31e_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[3] = { 0xaa, 0x2d, 0xd4 };
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned len = bb->bits_per_row[r];
        const unsigned start = search(bb->bb[r], len, 0, pre, 24);
        if (start == len || start + 24 + 7 * 8 > len) continue;
        uint8_t b[18] = { 0 };
        const unsigned avail = len - (start + 24);
        extract_bytes(bb->bb[r], start + 24, b, avail < 18 * 8 ? avail : 18 * 8);
        if (b[0] != 0x30 && b[0] != 0x37) continue;
        if (lr433_crc8(b, 6, 0x31, 0x00)) continue;
        if ((uint8_t)(add_bytes(b, 6) - b[6])) continue;
        lr433_msg_t *m = msg_new(c, LR433_P_AMBIENT_WH31E,
                                 b[0] == 0x30 ? "AmbientWeather-WH31E" : "AmbientWeather-WH31B");
        snprintf(m->id, sizeof(m->id), "%d", b[1]);
        m->channel = (int8_t)(((b[2] & 0x70) >> 4) + 1);
        m->battery_ok = !((b[2] & 0x04) >> 2);
        m->temp_c = ((((b[2] & 0x03) << 8) | b[3]) - 400) * 0.1f;
        m->humidity = (float)b[4];
        found += msg_done(c);
    }
    return found;
}

/* fineoffset.c: WH24, WH65 and kin (the outdoor arrays of the 915 MHz
   stations). Temperature and humidity only: the wind and rain scale depends
   on which model it is, which upstream judges by the row's exact length. */
static int wh24_cb(ctx_t *c, bitbuf_t *bb)
{
    static const uint8_t pre[3] = { 0xAA, 0x2D, 0xD4 };
    int found = 0;
    for (unsigned r = 0; r < bb->num_rows; r++) {
        const unsigned len = bb->bits_per_row[r];
        const unsigned at = search(bb->bb[r], len, 0, pre, 24);
        if (at == len) continue;
        const unsigned off = at + 24;
        if (off + 17 * 8 > len) continue;
        uint8_t b[17];
        extract_bytes(bb->bb[r], off, b, 17 * 8);
        if (b[0] != 0x24) continue;
        if (lr433_crc8(b, 16, 0x31, 0x00) != 0 || (uint8_t)add_bytes(b, 16) != b[16]) continue;
        lr433_msg_t *m = msg_new(c, LR433_P_FINEOFFSET_WH24, "Fineoffset-WH24");
        snprintf(m->id, sizeof(m->id), "%d", b[1]);
        m->battery_ok = !((b[3] & 0x08) >> 3);
        const int temp_raw = (b[3] & 0x07) << 8 | b[4];
        if (temp_raw != 0x7ff) m->temp_c = (temp_raw - 400) * 0.1f;
        if (b[5] != 0xff) m->humidity = (float)b[5];
        found += msg_done(c);
    }
    return found;
}

/* -------------------------------------------------------------- slicers -- */

static int us_to_s(uint32_t us, uint32_t rate)
{
    return (int)lrintf((float)us * (float)rate / 1e6f);
}

static int account(ctx_t *c, const decoder_t *d, bitbuf_t *bits)
{
    const int ret = d->cb(c, bits);
    bb_clear(bits);
    return ret > 0 ? ret : 0;
}

/* pulse_slicer_pcm */
static int slice_pcm(const lr433_pulses_t *p, const decoder_t *d, ctx_t *c, bitbuf_t *bits)
{
    const int s_short = us_to_s(d->short_us, p->rate);
    const int s_long = us_to_s(d->long_us, p->rate);
    const int s_reset = us_to_s(d->reset_us, p->rate);
    const int s_gap = us_to_s(d->gap_us, p->rate);
    int s_tol = us_to_s(d->tol_us, p->rate);
    if (s_short <= 0 || s_long <= 0 || s_reset <= 0) return 0;
    float f_short = 1.0f / ((float)d->short_us * p->rate / 1e6f);
    float f_long = 1.0f / ((float)d->long_us * p->rate / 1e6f);
    int events = 0;
    bb_clear(bits);
    const int gap_limit = s_gap ? s_gap : s_reset;
    const int max_zeros = gap_limit / s_long;
    if (s_tol <= 0) s_tol = s_long / 4;

    int min_count = s_short == s_long ? 12 : 4;
    int preamble_len = 0;
    /* RZ: a run of bit-wide toggles tunes the bit period. */
    for (int n = 0; s_short != s_long && n < p->n; ++n) {
        int swidth = 0, lwidth = 0, count = 0;
        while (n < p->n && p->pulse[n] >= s_short - s_tol && p->pulse[n] <= s_short + s_tol &&
               p->pulse[n] + p->gap[n] >= s_long - s_tol && p->pulse[n] + p->gap[n] <= s_long + s_tol) {
            swidth += p->pulse[n];
            lwidth += p->pulse[n] + p->gap[n];
            count++;
            n++;
        }
        if (count >= min_count) {
            f_long = (float)count / lwidth;
            f_short = (float)count / swidth;
            min_count = count;
            preamble_len = count;
        }
    }
    /* RZ bits within tolerance anywhere. */
    {
        int sw = 0, lw = 0, cnt = 0;
        for (int n = 0; preamble_len == 0 && s_short != s_long && n < p->n; ++n) {
            if (p->pulse[n] >= s_short - s_tol && p->pulse[n] <= s_short + s_tol &&
                p->pulse[n] + p->gap[n] >= s_long - s_tol && p->pulse[n] + p->gap[n] <= s_long + s_tol) {
                sw += p->pulse[n];
                lw += p->pulse[n] + p->gap[n];
                cnt++;
            }
        }
        if (cnt > 8) {
            f_long = (float)cnt / lw;
            f_short = (float)cnt / sw;
        }
    }
    /* NRZ: a preamble of single-width pulses and gaps tunes it. */
    for (int n = 0; s_short == s_long && n < p->n; ++n) {
        int width = 0, count = 0;
        while (n < p->n && (int)(p->pulse[n] * f_short + 0.5f) == 1 && (int)(p->gap[n] * f_long + 0.5f) == 1) {
            width += p->pulse[n] + p->gap[n];
            count += 2;
            n++;
        }
        if (count >= min_count) {
            f_short = f_long = (float)count / width;
            min_count = count;
            preamble_len = count;
        }
    }
    /* NRZ pulses and gaps of one or two bits within tolerance anywhere. */
    {
        int width = 0, cnt = 0;
        for (int n = 0; preamble_len == 0 && s_short == s_long && n < p->n; ++n) {
            if (p->pulse[n] >= s_short - s_tol && p->pulse[n] <= s_short + s_tol) { width += p->pulse[n]; cnt += 1; }
            if (p->pulse[n] >= 2 * s_short - s_tol && p->pulse[n] <= 2 * s_short + s_tol) { width += p->pulse[n]; cnt += 2; }
            if (p->gap[n] >= s_long - s_tol && p->gap[n] <= s_long + s_tol) { width += p->gap[n]; cnt += 1; }
            if (p->gap[n] >= 2 * s_long - s_tol && p->gap[n] <= 2 * s_long + s_tol) { width += p->gap[n]; cnt += 2; }
        }
        if (cnt > 20) f_short = f_long = (float)cnt / width;
    }

    for (int n = 0; n < p->n; ++n) {
        const int highs = (int)(p->pulse[n] * f_short + 0.5f);
        int lows = (int)((p->gap[n] + s_short - s_long) * f_long + 0.5f);
        for (int i = 0; i < highs; ++i) bb_add_bit(bits, 1);
        if (lows > max_zeros) lows = max_zeros;
        for (int i = 0; i < lows; ++i) bb_add_bit(bits, 0);
        if (s_short != s_long && (p->pulse[n] > s_short + s_tol || p->pulse[n] < s_short - s_tol)) {
            bb_clear(bits);
        } else if (p->gap[n] > gap_limit && p->gap[n] <= s_reset) {
            bb_add_row(bits);
        }
        if ((n == p->n - 1 || p->gap[n] > s_reset) && (bits->bits_per_row[0] > 0 || bits->num_rows > 1))
            events += account(c, d, bits);
    }
    return events;
}

/* pulse_slicer_pwm */
static int slice_pwm(const lr433_pulses_t *p, const decoder_t *d, ctx_t *c, bitbuf_t *bits)
{
    const int s_short = us_to_s(d->short_us, p->rate);
    const int s_long = us_to_s(d->long_us, p->rate);
    const int s_reset = us_to_s(d->reset_us, p->rate);
    const int s_gap = us_to_s(d->gap_us, p->rate);
    const int s_sync = us_to_s(d->sync_us, p->rate);
    const int s_tol = us_to_s(d->tol_us, p->rate);
    if (s_short <= 0 || s_long <= 0 || s_reset <= 0) return 0;
    int events = 0;
    bb_clear(bits);
    int one_l, one_u, zero_l, zero_u, sync_l = 0, sync_u = 0;
    if (s_tol > 0) {
        one_l = s_short - s_tol; one_u = s_short + s_tol;
        zero_l = s_long - s_tol; zero_u = s_long + s_tol;
        if (s_sync > 0) { sync_l = s_sync - s_tol; sync_u = s_sync + s_tol; }
    } else if (s_sync <= 0) {
        one_l = 0; one_u = (s_short + s_long) / 2 + 1;
        zero_l = one_u - 1; zero_u = 0x7fffffff;
    } else if (s_sync < s_short) {
        sync_l = 0; sync_u = (s_sync + s_short) / 2 + 1;
        one_l = sync_u - 1; one_u = (s_short + s_long) / 2 + 1;
        zero_l = one_u - 1; zero_u = 0x7fffffff;
    } else if (s_sync < s_long) {
        one_l = 0; one_u = (s_short + s_sync) / 2 + 1;
        sync_l = one_u - 1; sync_u = (s_sync + s_long) / 2 + 1;
        zero_l = sync_u - 1; zero_u = 0x7fffffff;
    } else {
        one_l = 0; one_u = (s_short + s_long) / 2 + 1;
        zero_l = one_u - 1; zero_u = (s_long + s_sync) / 2 + 1;
        sync_l = zero_u - 1; sync_u = 0x7fffffff;
    }
    for (int n = 0; n < p->n; ++n) {
        const int w = p->pulse[n];
        if (w > one_l && w < one_u) bb_add_bit(bits, 1);
        else if (w > zero_l && w < zero_u) bb_add_bit(bits, 0);
        else if (w > sync_l && w < sync_u) bb_add_sync(bits);
        else if (w <= one_l) { /* a spurious short pulse */ }
        else bb_add_row(bits);
        if ((n == p->n - 1 || p->gap[n] > s_reset) && bits->num_rows > 0)
            events += account(c, d, bits);
        else if (s_gap > 0 && p->gap[n] > s_gap && bits->num_rows > 0 &&
                 bits->bits_per_row[bits->num_rows - 1] > 0)
            bb_add_row(bits);
    }
    return events;
}

/* pulse_slicer_ppm */
static int slice_ppm(const lr433_pulses_t *p, const decoder_t *d, ctx_t *c, bitbuf_t *bits)
{
    const int s_short = us_to_s(d->short_us, p->rate);
    const int s_long = us_to_s(d->long_us, p->rate);
    const int s_reset = us_to_s(d->reset_us, p->rate);
    const int s_gap = us_to_s(d->gap_us, p->rate);
    const int s_sync = us_to_s(d->sync_us, p->rate);
    const int s_tol = us_to_s(d->tol_us, p->rate);
    if (s_short <= 0 || s_long <= 0 || s_reset <= 0) return 0;
    int events = 0;
    bb_clear(bits);
    int zero_l, zero_u, one_l, one_u, sync_l = 0, sync_u = 0;
    if (s_tol > 0) {
        zero_l = s_short - s_tol; zero_u = s_short + s_tol;
        one_l = s_long - s_tol; one_u = s_long + s_tol;
        if (s_sync > 0) { sync_l = s_sync - s_tol; sync_u = s_sync + s_tol; }
    } else {
        zero_l = 0; zero_u = (s_short + s_long) / 2 + 1;
        one_l = zero_u - 1; one_u = s_gap ? s_gap : s_reset;
    }
    for (int n = 0; n < p->n; ++n) {
        const int g = p->gap[n];
        if (g > zero_l && g < zero_u) bb_add_bit(bits, 0);
        else if (g > one_l && g < one_u) bb_add_bit(bits, 1);
        else if (g > sync_l && g < sync_u) bb_add_sync(bits);
        else if (g < s_reset) bb_add_row(bits);
        if ((n == p->n - 1 || g >= s_reset) && (bits->bits_per_row[0] > 0 || bits->num_rows > 1))
            events += account(c, d, bits);
    }
    return events;
}

/* pulse_slicer_manchester_zerobit */
static int slice_mc_zerobit(const lr433_pulses_t *p, const decoder_t *d, ctx_t *c, bitbuf_t *bits)
{
    const int s_short = us_to_s(d->short_us, p->rate);
    const int s_reset = us_to_s(d->reset_us, p->rate);
    const int s_tol = us_to_s(d->tol_us, p->rate);
    if (s_short <= 0 || s_reset <= 0) return 0;
    int events = 0, since = 0;
    bb_clear(bits);
    bb_add_bit(bits, 0);              /* the first rising edge is a zero */
    for (int n = 0; n < p->n; ++n) {
        const int pw = p->pulse[n], gw = p->gap[n];
        if (s_tol > 0 && (pw < s_short - s_tol || pw > s_short * 2 + s_tol ||
                          gw < s_short - s_tol || gw > s_short * 2 + s_tol)) {
            if (pw > s_short * 1.5f && pw <= s_short * 2 + s_tol) bb_add_bit(bits, 1);
            bb_add_row(bits);
            bb_add_bit(bits, 0);
            since = 0;
        } else if (pw + since > s_short * 1.5f) {
            bb_add_bit(bits, 1);      /* a falling data edge */
            since = 0;
        } else {
            since += pw;
        }
        if ((n == p->n - 1 || gw > s_reset) && bits->num_rows > 0) {
            events += account(c, d, bits);
            bb_add_bit(bits, 0);
            since = 0;
        } else if (gw + since > s_short * 1.5f) {
            bb_add_bit(bits, 0);      /* a rising data edge */
            since = 0;
        } else {
            since += gw;
        }
    }
    return events;
}

/* The OOK devices, then the FSK ones. Widths as upstream gives them. */
static const decoder_t OOK_DEVS[] = {
    { MOD_MC_ZEROBIT, 120, 0, 0, 0, 480, 0, schrader_cb },
    { MOD_MC_ZEROBIT, 123, 0, 0, 0, 300, 0, schrader_eg53ma4_cb },
    { MOD_PCM, 120, 120, 0, 0, 480, 0, schrader_smd3ma4_cb },
    { MOD_MC_ZEROBIT, 120, 0, 0, 0, 15600, 0, gm_cb },
    { MOD_PWM, 220, 408, 620, 500, 4000, 0, acurite_txr_cb },
    { MOD_PWM, 208, 417, 833, 625, 1700, 0, lacrosse_tx141x_cb },
    { MOD_MC_ZEROBIT, 500, 0, 0, 0, 2400, 0, f007th_cb },
    { MOD_PWM, 500, 1500, 0, 0, 1200, 0, fineoffset_wh2_cb },
};

static const decoder_t FSK_DEVS[] = {
    { MOD_PCM, 52, 52, 0, 0, 150, 0, ford_cb },
    { MOD_PCM, 52, 52, 0, 0, 150, 0, toyota_cb },
    { MOD_PCM, 100, 100, 0, 0, 250, 0, pmv107j_cb },
    { MOD_PCM, 52, 52, 0, 0, 150, 0, citroen_cb },
    { MOD_PCM, 52, 52, 0, 0, 150, 0, hyundai_vdo_cb },
    { MOD_PCM, 50, 50, 0, 0, 200, 0, kia_cb },
    { MOD_PCM, 49, 49, 0, 0, 200, 0, elantra_cb },
    { MOD_PCM, 56, 56, 0, 1800, 1500, 0, wh31e_cb },
    { MOD_PCM, 58, 58, 0, 0, 20000, 0, wh24_cb },
};

static int run_dev(const lr433_pulses_t *p, const decoder_t *d, ctx_t *c, bitbuf_t *bits)
{
    switch (d->mod) {
    case MOD_PCM: return slice_pcm(p, d, c, bits);
    case MOD_PWM: return slice_pwm(p, d, c, bits);
    case MOD_PPM: return slice_ppm(p, d, c, bits);
    default:      return slice_mc_zerobit(p, d, c, bits);
    }
}

/* -------------------------------------------------------------------- ERT -- */

/* The ERT session's detector is the 7 bits 1010100 as Manchester chips
   (10011001100101), which all three messages carry near their start: SCM at
   bits 7-13 of its 0x1F2A60 preamble, SCM+ at bits 6-12 of its 0x16A3 sync,
   IDM where its 0x5555 preamble meets that sync. What came before the match
   is known, so it is put back, and the rest is read as Manchester pairs. */

typedef struct {
    uint16_t bits;
    uint8_t b[128];
} long_row_t;

static void lrow_add(long_row_t *r, int bit)
{
    if (r->bits >= sizeof(r->b) * 8) return;
    if (bit) r->b[r->bits >> 3] |= (uint8_t)(0x80 >> (r->bits & 7));
    r->bits++;
}

static void lrow_put(long_row_t *r, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) lrow_add(r, (int)((v >> i) & 1u));
}

static void ert_scm(ctx_t *c, const long_row_t *data)
{
    if (data->bits < 96) return;
    const uint8_t *b = data->b;
    if (!b[0] && !b[1] && !b[2] && !b[3]) return;
    if (lr433_crc16(&b[2], 10, 0x6F63, 0)) return;
    lr433_msg_t *m = msg_new(c, LR433_P_ERT_SCM, "ERT-SCM");
    const uint32_t id = ((uint32_t)(b[2] & 0x06) << 23) | ((uint32_t)b[7] << 16) | (b[8] << 8) | b[9];
    snprintf(m->id, sizeof(m->id), "%lu", (unsigned long)id);
    m->consumption = ((int64_t)b[4] << 16) | (b[5] << 8) | b[6];
    msg_done(c);
}

static void ert_scmplus(ctx_t *c, const long_row_t *row)
{
    static const uint8_t sync[3] = { 0x16, 0xA3, 0x1E };
    const unsigned at = search(row->b, row->bits, 0, sync, 24);
    if (at >= row->bits || row->bits - at < 128) return;
    uint8_t b[16];
    extract_bytes(row->b, at, b, 128);
    if (lr433_crc16(&b[2], 12, 0x1021, 0x0971) != (uint16_t)(b[14] << 8 | b[15])) return;
    lr433_msg_t *m = msg_new(c, LR433_P_ERT_SCMPLUS, "SCMplus");
    snprintf(m->id, sizeof(m->id), "%lu",
             (unsigned long)(((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | (b[6] << 8) | b[7]));
    m->consumption = (int64_t)(((uint32_t)b[8] << 24) | ((uint32_t)b[9] << 16) | (b[10] << 8) | b[11]);
    msg_done(c);
}

static void ert_idm(ctx_t *c, const long_row_t *row)
{
    static const uint8_t sync[3] = { 0x16, 0xA3, 0x1C };
    const unsigned at = search(row->b, row->bits, 0, sync, 24);
    if (at >= row->bits || row->bits - at < 720) return;
    uint8_t b[90];
    extract_bytes(row->b, at, b, 720);
    if (lr433_crc16(&b[2], 86, 0x1021, 0xD895) != (uint16_t)(b[88] << 8 | b[89])) return;
    lr433_msg_t *m = msg_new(c, LR433_P_ERT_IDM, "IDM");
    snprintf(m->id, sizeof(m->id), "%lu",
             (unsigned long)(((uint32_t)b[7] << 24) | ((uint32_t)b[8] << 16) | (b[9] << 8) | b[10]));
    m->consumption = (int64_t)(((uint32_t)b[27] << 24) | ((uint32_t)b[28] << 16) | (b[29] << 8) | b[30]);
    msg_done(c);
}

static void ert_decode(const lr433_capture_t *cap, ctx_t *c)
{
    long_row_t scm = { 0 }, rest = { 0 };
    lrow_put(&scm, 0x7C, 7);           /* 1111100 */
    lrow_put(&scm, 0x54, 7);           /* 1010100, the detector */
    lrow_put(&rest, 0x05, 6);          /* 000101 */
    lrow_put(&rest, 0x54, 7);
    for (int i = 0; i + 1 < cap->bits; i += 2) {
        const unsigned a = bit_at(cap->data, (unsigned)i), b = bit_at(cap->data, (unsigned)i + 1);
        if (a == b) break;
        lrow_add(&scm, (int)a);
        lrow_add(&rest, (int)a);
    }
    ert_scm(c, &scm);
    ert_scmplus(c, &rest);
    ert_idm(c, &rest);
}

/* ---------------------------------------------------------------- entry -- */

int lr433_decode(const lr433_capture_t *cap, lr433_work_t *w, lr433_msg_t *out, int max)
{
    ctx_t c = { .out = out, .max = max, .n = 0 };
    if (!cap || !w || !out || max <= 0 || !cap->rate) return 0;
    if (cap->kind == LR433_IN_ERT) {
        ert_decode(cap, &c);
        return c.n;
    }
    const bool fsk = cap->kind == LR433_IN_FSK;
    const decoder_t *devs = fsk ? FSK_DEVS : OOK_DEVS;
    const int ndev = fsk ? (int)(sizeof(FSK_DEVS) / sizeof(FSK_DEVS[0]))
                         : (int)(sizeof(OOK_DEVS) / sizeof(OOK_DEVS[0]));
    for (int pol = 0; pol < (fsk ? 2 : 1); pol++) {
        lr433_pulses(cap, pol != 0, &w->p);
        if (!w->p.n) continue;
        for (int i = 0; i < ndev; i++) run_dev(&w->p, &devs[i], &c, &w->bits);
    }
    return c.n;
}

/* ---------------------------------------------------------------- names -- */

static const struct { const char *name, *label; lr433_class_t cls; } PROTOS[LR433_P_COUNT] = {
    [LR433_P_SCHRADER]         = { "Schrader", "Schrader", LR433_TPMS },
    [LR433_P_SCHRADER_EG53MA4] = { "Schrader-EG53MA4", "Schr-EG53", LR433_TPMS },
    [LR433_P_SCHRADER_SMD3MA4] = { "Schrader-SMD3MA4", "Schr-SMD3", LR433_TPMS },
    [LR433_P_GM_AFTERMARKET]   = { "GM-Aftermarket", "GM-Aftmkt", LR433_TPMS },
    [LR433_P_FORD]             = { "Ford", "Ford", LR433_TPMS },
    [LR433_P_TOYOTA]           = { "Toyota", "Toyota", LR433_TPMS },
    [LR433_P_PMV107J]          = { "PMV-107J", "PMV-107J", LR433_TPMS },
    [LR433_P_CITROEN]          = { "Citroen", "Citroen", LR433_TPMS },
    [LR433_P_HYUNDAI_VDO]      = { "Hyundai-VDO", "HyunVDO", LR433_TPMS },
    [LR433_P_KIA]              = { "Kia", "Kia", LR433_TPMS },
    [LR433_P_ELANTRA2012]      = { "Elantra2012", "Elantra", LR433_TPMS },
    [LR433_P_ACURITE_TOWER]    = { "Acurite-Tower", "Acu-Tower", LR433_SENSOR },
    [LR433_P_ACURITE_5N1]      = { "Acurite-5n1", "Acu-5n1", LR433_SENSOR },
    [LR433_P_LACROSSE_TX141]   = { "LaCrosse-TX141", "LaCrosse", LR433_SENSOR },
    [LR433_P_AMBIENT_F007TH]   = { "Ambientweather-F007TH", "F007TH", LR433_SENSOR },
    [LR433_P_FINEOFFSET_WH2]   = { "Fineoffset-WH2", "FO-WH2", LR433_SENSOR },
    [LR433_P_AMBIENT_WH31E]    = { "AmbientWeather-WH31E", "WH31E", LR433_SENSOR },
    [LR433_P_FINEOFFSET_WH24]  = { "Fineoffset-WH24", "FO-WH24", LR433_SENSOR },
    [LR433_P_ERT_SCM]          = { "ERT-SCM", "ERT-SCM", LR433_METER },
    [LR433_P_ERT_SCMPLUS]      = { "SCMplus", "ERT-SCM+", LR433_METER },
    [LR433_P_ERT_IDM]          = { "IDM", "ERT-IDM", LR433_METER },
};

const char *lr433_proto_name(int proto)
{
    return proto >= 0 && proto < LR433_P_COUNT ? PROTOS[proto].name : "?";
}

const char *lr433_proto_label(int proto)
{
    return proto >= 0 && proto < LR433_P_COUNT ? PROTOS[proto].label : "?";
}

lr433_class_t lr433_proto_class(int proto)
{
    return proto >= 0 && proto < LR433_P_COUNT ? PROTOS[proto].cls : LR433_SENSOR;
}
