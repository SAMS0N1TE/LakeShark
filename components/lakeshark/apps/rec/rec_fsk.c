/* See rec_fsk.h. */

#include "rec_fsk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI    6.28318530718f
#define LP_ALPHA  0.25f          /* ~4 samples: well inside one bit */
#define TONE_A    (1.0f / 64.0f)
#define HYST      0.15f          /* of half the tone separation */
/* A burst that follows at least this much silence starts with no tones.
   Noise over the gate while armed is a carrier to this slicer; without
   this, one noise blip left a tone far off and sliced every later burst
   against a wrong centre. */
#define FORGET_S  0.001f
/* Readings to skip at the start of a burst while the filter settles, and
   how long a new tone must hold before it is believed: a few samples,
   well inside one bit at any rate REC can record. */
#define SETTLE_N  4u
#define CONFIRM_N 6u
/* A tone not seen for this long during one burst is dropped, so a wrong
   one learned mid-burst cannot hold the centre for the rest of it. */
#define STALE_S   0.02f

void rec_fsk_init(rec_fsk_t *s, float fs)
{
    memset(s, 0, sizeof(*s));
    s->fs = fs;
    s->fresh = true;
}

void rec_fsk_clear_stats(rec_fsk_t *s)
{
    s->hi_sum = s->lo_sum = 0.0;
    s->hi_n = s->lo_n = 0;
}

/* Until both tones have been seen the centre is provisional: just inside
   the one that has, so a tuner far enough off that both tones sit on the
   same side of zero still finds the second one. */
#define PROVISIONAL_HZ 2000.0f

static float center_of(const rec_fsk_t *s)
{
    if (s->have_hi && s->have_lo) return 0.5f * (s->tone_hi + s->tone_lo);
    if (s->have_hi) return s->tone_hi - PROVISIONAL_HZ;
    if (s->have_lo) return s->tone_lo + PROVISIONAL_HZ;
    return 0.0f;
}

static void forget_tones(rec_fsk_t *s)
{
    s->have_hi = s->have_lo = false;
    s->cand_n = 0;
    s->cand_side = 0;
}

/* x has held on one side of the only known tone; true once it has for
   CONFIRM_N readings in a row. */
static bool confirmed(rec_fsk_t *s, int8_t side)
{
    if (s->cand_side != side) { s->cand_side = side; s->cand_n = 0; }
    return ++s->cand_n >= CONFIRM_N;
}

bool rec_fsk_step(rec_fsk_t *s, int i, int q, bool carrier)
{
    if (!carrier) {
        s->have_prev = false;
        s->fresh = true;
        s->bit = false;
        if (s->off_n < UINT32_MAX) s->off_n++;
        return false;
    }
    if (s->off_n) {
        if (s->off_n >= (uint32_t)(FORGET_S * s->fs)) forget_tones(s);
        s->off_n = 0;
        s->on_n = 0;
    }
    s->on_n++;
    if (!s->have_prev) {
        s->pi = i; s->pq = q;
        s->have_prev = true;
        return s->bit;
    }

    const float cross = (float)(s->pi * q - s->pq * i);
    const float dot   = (float)(s->pi * i + s->pq * q);
    s->pi = i; s->pq = q;
    const float f = atan2f(cross, dot) * (s->fs / TWO_PI);
    /* Start each burst's filter on its first reading rather than ramping
       up from wherever the last burst left it. */
    if (s->fresh) { s->f_lp = f; s->fresh = false; }
    else s->f_lp += (f - s->f_lp) * LP_ALPHA;

    /* The centre is the midpoint of the two tones, each averaged only over
       its own samples. A long run of one bit then pulls that tone's
       estimate towards where it already is and the centre stays put,
       which an average of every sample would not. */
    const float c = center_of(s);
    const float half = (s->have_hi && s->have_lo)
                       ? 0.5f * (s->tone_hi - s->tone_lo) : 0.0f;
    const float h = half > 0.0f ? HYST * half : 0.0f;

    /* With one tone known the bit stays on its side until the other is
       confirmed: a provisional centre sits inside the noise and chatters. */
    if (s->have_hi && s->have_lo) {
        if (s->f_lp > c + h)      s->bit = true;
        else if (s->f_lp < c - h) s->bit = false;
    } else if (s->have_hi || s->have_lo) {
        s->bit = s->have_hi;
    } else {
        s->bit = s->f_lp > 0.0f;
    }

    const float x = s->f_lp;
    const uint32_t now = s->on_n;
    if (now <= SETTLE_N) {
        /* the filter is still settling on this burst's first readings */
    } else if (!s->have_hi && !s->have_lo) {
        if (x > 0.0f) { s->tone_hi = x; s->have_hi = true; s->hi_seen = now; }
        else          { s->tone_lo = x; s->have_lo = true; s->lo_seen = now; }
        s->cand_side = 0;
    } else if (!s->have_lo) {
        /* One tone known: a reading that holds well away from it is the
           other one, and if it lies above, what we called the upper tone
           was the lower. */
        if (x < s->tone_hi - PROVISIONAL_HZ) {
            if (confirmed(s, -1)) { s->tone_lo = x; s->have_lo = true; s->lo_seen = now; }
        } else if (x > s->tone_hi + PROVISIONAL_HZ) {
            if (confirmed(s, +1)) {
                s->tone_lo = s->tone_hi; s->lo_seen = s->hi_seen;
                s->tone_hi = x; s->hi_seen = now; s->have_lo = true;
            }
        } else {
            s->cand_side = 0;
            s->tone_hi += (x - s->tone_hi) * TONE_A;
            s->hi_seen = now;
        }
    } else if (!s->have_hi) {
        if (x > s->tone_lo + PROVISIONAL_HZ) {
            if (confirmed(s, +1)) { s->tone_hi = x; s->have_hi = true; s->hi_seen = now; }
        } else if (x < s->tone_lo - PROVISIONAL_HZ) {
            if (confirmed(s, -1)) {
                s->tone_hi = s->tone_lo; s->hi_seen = s->lo_seen;
                s->tone_lo = x; s->lo_seen = now; s->have_hi = true;
            }
        } else {
            s->cand_side = 0;
            s->tone_lo += (x - s->tone_lo) * TONE_A;
            s->lo_seen = now;
        }
    } else {
        if (x > c) { s->tone_hi += (x - s->tone_hi) * TONE_A; s->hi_seen = now; }
        else       { s->tone_lo += (x - s->tone_lo) * TONE_A; s->lo_seen = now; }
        const uint32_t stale = (uint32_t)(STALE_S * s->fs);
        if (now - s->lo_seen > stale)      s->have_lo = false;
        else if (now - s->hi_seen > stale) s->have_hi = false;
    }

    /* Statistics only from settled samples: half-way or more out towards
       a tone, so the ramps between bits do not drag the measure in. */
    if (half > 0.0f) {
        const float d = s->f_lp - c;
        if (d > 0.5f * half)       { s->hi_sum += d;  s->hi_n++; }
        else if (d < -0.5f * half) { s->lo_sum -= d;  s->lo_n++; }
    }
    return s->bit;
}

uint32_t rec_fsk_deviation(const rec_fsk_t *s)
{
    if (!s->hi_n || !s->lo_n) return 0;
    const double dev = 0.5 * (s->hi_sum / s->hi_n + s->lo_sum / s->lo_n);
    return dev > 0.0 ? (uint32_t)(dev + 0.5) : 0;
}

uint32_t rec_fsk_bitrate(const int32_t *edges, int n, uint32_t min_us)
{
    if (!edges || n < 2) return 0;

    /* The shortest run is one bit, but a single run can be clipped by a
       sample or two of slicer latency. Average every run within half a bit
       of the shortest rather than trusting the one extreme. The first and
       last runs are left out: they are bounded by the burst, not the
       clock. */
    uint32_t shortest = 0;
    for (int k = 1; k + 1 < n; k++) {
        const uint32_t v = (uint32_t)(edges[k] < 0 ? -edges[k] : edges[k]);
        if (v < min_us) continue;
        if (!shortest || v < shortest) shortest = v;
    }
    if (!shortest) return 0;

    uint64_t sum = 0;
    uint32_t cnt = 0;
    for (int k = 1; k + 1 < n; k++) {
        const uint32_t v = (uint32_t)(edges[k] < 0 ? -edges[k] : edges[k]);
        if (v < shortest || v > shortest + shortest / 2) continue;
        sum += v;
        cnt++;
    }
    if (!cnt) return 0;
    const uint64_t bit_us = (sum + cnt / 2) / cnt;
    return bit_us ? (uint32_t)((1000000ull + bit_us / 2) / bit_us) : 0;
}

bool rec_fsk_preset_is_fsk(const char *preset, const char *custom,
                           uint32_t *dev_hz)
{
    if (dev_hz) *dev_hz = 0;
    if (!preset) return false;
    /* The Flipper's stock FSK presets name their deviation. */
    if (strstr(preset, "2FSKDev238")) { if (dev_hz) *dev_hz = 2380;  return true; }
    if (strstr(preset, "2FSKDev476")) { if (dev_hz) *dev_hz = 47607; return true; }
    if (!strstr(preset, "Custom") || !custom) return false;

    /* Register pairs until the 00 00 terminator. MDMCFG2 (0x12) bits 6:4
       are MOD_FORMAT, 000 being 2-FSK; DEVIATN (0x15) is E<<4 | M. */
    bool fsk = false;
    const char *p = custom;
    char *end;
    for (;;) {
        const long reg = strtol(p, &end, 16);
        if (end == p) break;
        p = end;
        const long val = strtol(p, &end, 16);
        if (end == p) break;
        p = end;
        if (reg == 0 && val == 0) break;
        if (reg == 0x12) fsk = ((val >> 4) & 0x7) == 0;
        if (reg == 0x15 && dev_hz) {
            const uint32_t e = ((uint32_t)val >> 4) & 7u, m = (uint32_t)val & 7u;
            const uint64_t exact = 26000000ull * (8u + m) * (1u << e);
            *dev_hz = (uint32_t)((exact + (1u << 16)) >> 17);
        }
    }
    if (!fsk && dev_hz) *dev_hz = 0;
    return fsk;
}
