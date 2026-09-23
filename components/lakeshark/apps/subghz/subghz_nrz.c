#include "subghz_nrz.h"

#include <stdio.h>
#include <string.h>

/* A capture carries the receiver's own chatter between frames - runs of tens
   of microseconds, far below the bit period. They are not signal and they
   must not drag the unit estimate down. */
#define MIN_UNIT_US   120
#define MAX_UNIT_US   4000
#define MAX_RUN_UNITS 40

#define MAX_BITS      4096

static int rd(const uint8_t *b, int i) { return (b[i >> 3] >> (7 - (i & 7))) & 1; }
static void wr(uint8_t *b, int i, int v)
{
    const uint8_t m = (uint8_t)(1u << (7 - (i & 7)));
    if (v) b[i >> 3] |= m; else b[i >> 3] &= (uint8_t)~m;
}

/* The bit period is the shortest duration that the rest are multiples of.
   Taking the most common duration finds it directly on this encoding,
   because a payload spends most of its runs at one unit. */
static int estimate_unit(const int32_t *p, int n)
{
    /* 32 us buckets over the plausible range: fine enough to separate 415
       from 830, coarse enough that a transmitter's drift stays in one bin. */
    enum { BUCKETS = MAX_UNIT_US / 32 + 1 };
    int count[BUCKETS];
    memset(count, 0, sizeof(count));

    for (int i = 0; i < n; i++) {
        const int d = p[i] < 0 ? -p[i] : p[i];
        if (d < MIN_UNIT_US || d > MAX_UNIT_US) continue;
        count[d / 32]++;
    }
    int best = -1, best_n = 0;
    for (int b = 0; b < BUCKETS; b++)
        if (count[b] > best_n) { best_n = count[b]; best = b; }
    if (best < 0 || best_n < 16) return 0;

    /* Mean of the bucket, so the unit is not quantised to 32 us. */
    long sum = 0; int k = 0;
    const int lo = best * 32, hi = lo + 32;
    for (int i = 0; i < n; i++) {
        const int d = p[i] < 0 ? -p[i] : p[i];
        if (d >= lo && d < hi) { sum += d; k++; }
    }
    return k ? (int)(sum / k) : 0;
}

/* Edges to bits: a mark of N units is N ones, a space of N units is N zeros.
   A run that is not a whole number of units, or is longer than a frame gap,
   ends the run of bits rather than corrupting it. */
static int to_bits(const int32_t *p, int n, int unit, uint8_t *bits,
                   int max_bits, int *starts, int *n_starts, int max_starts)
{
    int nb = 0;
    *n_starts = 0;
    for (int i = 0; i < n && nb < max_bits; i++) {
        const int d = p[i] < 0 ? -p[i] : p[i];
        const int units = (d + unit / 2) / unit;
        if (units < 1 || units > MAX_RUN_UNITS) {
            /* A gap. The next bit begins a fresh frame. */
            if (*n_starts < max_starts) starts[(*n_starts)++] = nb;
            continue;
        }
        for (int k = 0; k < units && nb < max_bits; k++)
            wr(bits, nb++, p[i] > 0 ? 1 : 0);
    }
    return nb;
}

bool subghz_nrz_decode(const int32_t *pulse, int edges, subghz_nrz_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!pulse || edges < 32) return false;

    const int unit = estimate_unit(pulse, edges);
    if (unit <= 0) return false;

    static uint8_t bits[MAX_BITS / 8];
    memset(bits, 0, sizeof(bits));
    int starts[64], n_starts = 0;
    const int nb = to_bits(pulse, edges, unit, bits, MAX_BITS,
                           starts, &n_starts, 64);
    if (nb < 32) return false;

    /* Frames repeat. The period is the smallest shift at which the stream
       matches itself over a whole frame - which is what separates a payload
       from the noise either side of it. */
    int period = 0;
    for (int cand = 24; cand <= nb / 2 && !period; cand++) {
        int ok = 1;
        for (int i = 0; i + cand < nb && i < cand * 2; i++)
            if (rd(bits, i) != rd(bits, i + cand)) { ok = 0; break; }
        if (ok) period = cand;
    }
    if (!period) return false;

    int repeats = 1;
    for (int at = period; at + period <= nb; at += period) {
        int same = 1;
        for (int i = 0; i < period; i++)
            if (rd(bits, at + i) != rd(bits, at - period + i)) { same = 0; break; }
        if (!same) break;
        repeats++;
    }
    if (repeats < 2) return false;

    /* Drop the preamble: a run of ones to key the receiver, then alternating
       bits to train its slicer. Neither carries anything, and their lengths
       are properties of the transmitter's ramp rather than of the message.
       Measured on the 433.42 MHz captures: four ones, then 1010 thirty-odd
       times, then the payload. */
    int start = 0;
    while (start < period && rd(bits, start) == 1) start++;
    if (start > 0) start--;   /* the last one is the first training bit */
    while (start + 1 < period &&
           rd(bits, start) == 1 && rd(bits, start + 1) == 0)
        start += 2;
    /* Only a preamble if something is left to be a message. A payload that
       happens to open 1010 keeps it. */
    if (start > period - 8) start = 0;
    out->training = (uint16_t)start;

    int len = period - start;
    if (len > SUBGHZ_NRZ_MAX_BITS) len = SUBGHZ_NRZ_MAX_BITS;
    for (int i = 0; i < len; i++) wr(out->bits, i, rd(bits, start + i));

    out->n_bits = (uint16_t)len;
    out->unit_us = (uint16_t)unit;
    out->repeats = (uint8_t)(repeats > 255 ? 255 : repeats);
    return true;
}

size_t subghz_nrz_format(const subghz_nrz_t *in, char *out, size_t n)
{
    if (!out || n == 0) return 0;
    if (!in || !in->n_bits) { out[0] = 0; return 0; }

    int used = snprintf(out, n, "%u bits ", in->n_bits);
    if (used < 0 || (size_t)used >= n) { out[0] = 0; return 0; }

    /* As many nibbles as fit, so a long payload truncates rather than
       overflowing the caller's line. */
    const int nibbles = (in->n_bits + 3) / 4;
    for (int i = 0; i < nibbles && (size_t)used + 2 < n; i++) {
        int v = 0;
        for (int b = 0; b < 4; b++) {
            const int idx = i * 4 + b;
            v = (v << 1) | (idx < in->n_bits ? rd(in->bits, idx) : 0);
        }
        out[used++] = "0123456789ABCDEF"[v];
    }
    out[used] = 0;
    if ((size_t)used + 16 < n)
        used += snprintf(out + used, n - used, " x%u @%uus",
                         in->repeats, in->unit_us);
    return (size_t)used;
}
