#include "dmr.h"

#include <string.h>

#define RING_MASK (DMR_SS_RING - 1u)

/* Loop gains, in samples of correction per unit of normalised Gardner
   error. A random 4FSK stream gives a detector gain near 0.3 per sample of
   offset at 10 samples a symbol, so this settles a half-symbol error in
   roughly 30 symbols - inside the 54 symbols of payload ahead of a burst's
   sync - and jitters by well under a sample once settled. The period term
   absorbs a transmitter clock error; DMR allows a few ppm and a tuner
   reference adds its own, so it is held within 0.5%. */
#define TED_GAIN       0.30f
#define PERIOD_GAIN    0.0015f
#define PERIOD_SPAN    0.005f

/* Level tracking: the outer means move a sixteenth of the way per outer
   symbol. That follows a 1 kHz carrier step within about 30 symbols while
   averaging the noise on each estimate over about as many. */
#define LEVEL_GAIN     0.0625f
#define OUTER_GAIN     0.0625f
#define OUTER_FLOOR    0.05f

void dmr_symsync_reset(dmr_symsync_t *s, float samples_per_symbol)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->nominal = samples_per_symbol > 2.0f ? samples_per_symbol : 10.0f;
    s->period = s->nominal;
    s->t = s->period;
    s->outer_hi = s->outer_lo = 0.25f;
}

float dmr_symsync_sample(const dmr_symsync_t *s, uint32_t at, float frac)
{
    if (!s) return 0.0f;
    int whole = (int)frac;
    if ((float)whole > frac) whole--;               /* floor */
    const float f = frac - (float)whole;
    const uint32_t i0 = at + (uint32_t)whole;        /* modular on purpose */
    const uint32_t age = s->n - 1u - i0;
    if (age >= DMR_SS_RING - 1u) return 0.0f;
    const float x0 = s->ring[i0 & RING_MASK];
    const float x1 = age == 0 ? x0 : s->ring[(i0 + 1u) & RING_MASK];
    return x0 + (x1 - x0) * f;
}

void dmr_symsync_set_levels(dmr_symsync_t *s, float hi, float lo)
{
    if (!s || !(hi > lo)) return;
    s->hi = hi;
    s->lo = lo;
    s->outer_hi = s->outer_lo = 0.25f;
}

void dmr_symsync_shift(dmr_symsync_t *s, float samples)
{
    if (!s) return;
    const float lim = s->period * 0.5f;
    s->t += samples > lim ? lim : samples < -lim ? -lim : samples;
}

static void track_levels(dmr_symsync_t *s, float y)
{
    const float dc = 0.5f * (s->hi + s->lo);
    const float a  = 0.5f * (s->hi - s->lo);
    const float up = dc + a * (2.0f / 3.0f), dn = dc - a * (2.0f / 3.0f);

    const float is_hi = y > up ? 1.0f : 0.0f, is_lo = y < dn ? 1.0f : 0.0f;
    if (is_hi) s->hi += LEVEL_GAIN * (y - s->hi);
    if (is_lo) s->lo += LEVEL_GAIN * (y - s->lo);
    s->outer_hi += OUTER_GAIN * (is_hi - s->outer_hi);
    s->outer_lo += OUTER_GAIN * (is_lo - s->outer_lo);

    s->recent[s->recent_at] = y;
    s->recent_at = (s->recent_at + 1u) & 15u;
    if (s->symbols < 16u) return;

    /* A side of the eye wider than the signal slices that side's outer
       symbols as inner and so never receives the ones that would pull it
       in. A random stream is a quarter +3 and a quarter -3; a side this far
       below that has lost its level. But a run of one symbol starves a side
       just as well - an idle burst's payload is ninety-odd +1s - so the
       last 16 must reach across the centre both ways before either side is
       moved to them, or the eye would close onto the run. */
    if (s->outer_hi >= OUTER_FLOOR && s->outer_lo >= OUTER_FLOOR) return;
    float mx = s->recent[0], mn = s->recent[0];
    for (unsigned i = 1; i < 16u; i++) {
        if (s->recent[i] > mx) mx = s->recent[i];
        if (s->recent[i] < mn) mn = s->recent[i];
    }
    const bool spans = mx > dc && mn < dc;
    if (s->outer_hi < OUTER_FLOOR) {
        if (spans) s->hi = mx;
        s->outer_hi = 0.25f;
    }
    if (s->outer_lo < OUTER_FLOOR) {
        if (spans) s->lo = mn;
        s->outer_lo = 0.25f;
    }
}

bool dmr_symsync_push(dmr_symsync_t *s, float x, dmr_symbol_t *out)
{
    if (!s) return false;
    s->ring[s->n & RING_MASK] = x;
    s->n++;
    s->t -= 1.0f;
    if (s->t > 0.0f) return false;

    /* The strobe sits t samples (t in (-1, 0]) before the newest one, the
       Gardner midpoint half a period before that. */
    const uint32_t at = s->n - 1u;
    const float frac = s->t;
    const float y = dmr_symsync_sample(s, at, frac);
    const float m = dmr_symsync_sample(s, at, frac - 0.5f * s->period);

    const float dc = 0.5f * (s->hi + s->lo);
    const float a  = 0.5f * (s->hi - s->lo);
    if (a > 0.0f && s->symbols > 0) {
        /* Late strobes put the midpoint past the crossing, on the side of
           the newer symbol, which makes the product negative: pull in. */
        float e = (s->prev - y) * (m - dc) / (a * a);
        if (e > 1.0f) e = 1.0f;
        if (e < -1.0f) e = -1.0f;
        s->t += TED_GAIN * e;
        s->period += PERIOD_GAIN * e;
        const float span = s->nominal * PERIOD_SPAN;
        if (s->period > s->nominal + span) s->period = s->nominal + span;
        if (s->period < s->nominal - span) s->period = s->nominal - span;
    }
    s->t += s->period;
    s->prev = y;

    if (s->symbols == 0) { s->hi = y > 0.0f ? y : 1.0f; s->lo = y < 0.0f ? y : -1.0f; }
    if (out) {
        const float c = 0.5f * (s->hi + s->lo), h = 0.5f * (s->hi - s->lo);
        out->value  = y;
        out->center = c;
        out->umid   = c + h * (2.0f / 3.0f);
        out->lmid   = c - h * (2.0f / 3.0f);
        out->at     = at;
        out->frac   = frac;
    }
    track_levels(s, y);
    s->symbols++;
    return true;
}
