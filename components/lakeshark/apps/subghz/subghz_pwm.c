#include "subghz_pwm.h"

#include <stdio.h>
#include <string.h>

/* The same tolerance rec_decode_ook24() uses. Cheap receivers and cheap
   transmitters both drift, and a tighter window loses real frames. */
static bool near(int value, int expected)
{
    if (expected <= 0) return false;
    return value >= expected * 65 / 100 && value <= expected * 135 / 100;
}

/* A frame starts on a mark followed by a long space: the preamble. Its length
   in units is what tells one family from another, so it is measured rather
   than assumed - EV1527 is 31, PT2262 modules are commonly 23 or 31. */
static bool preamble_at(const int32_t *p, int i, int n, int *unit_out,
                        int *units_out)
{
    if (i + 1 >= n) return false;
    const int mark = p[i];
    if (mark < 100 || mark > 1000) return false;
    const int space = p[i + 1];
    if (space >= 0 || space < -50000) return false;

    const int units = (-space + mark / 2) / mark;
    if (units < 8 || units > 80) return false;
    if (!near(-space, mark * units)) return false;

    *unit_out = mark;
    *units_out = units;
    return true;
}

/* Read bits until the encoding stops holding. Every bit is a mark and a
   space, one short and one long, in whichever order the device uses; the
   order is fixed within a frame and is what `inverted` records. */
static int read_bits(const int32_t *p, int i, int n, int unit,
                     uint64_t *value_out, bool *inverted_out)
{
    uint64_t value = 0;
    int bits = 0;
    bool inverted = false;
    bool decided = false;

    while (i + 1 < n && bits < SUBGHZ_PWM_MAX_BITS) {
        const int hi = p[i], lo = p[i + 1];
        if (hi <= 0 || lo >= 0 || lo < -10000) break;

        const bool short_mark = near(hi, unit) && near(-lo, 3 * unit);
        const bool long_mark  = near(hi, 3 * unit) && near(-lo, unit);
        if (!short_mark && !long_mark) break;

        /* The first bit does not decide the polarity - both forms appear in
           every frame. A one is the long mark, which is EV1527's convention;
           `inverted` is left for a caller that meets the other one. */
        (void)decided;
        value = (value << 1) | (long_mark ? 1u : 0u);
        bits++;
        i += 2;
    }

    *value_out = value;
    *inverted_out = inverted;
    return bits;
}

bool subghz_pwm_decode(const int32_t *pulse, int edges, subghz_pwm_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!pulse || edges < 8) return false;

    uint64_t best_value = 0;
    int best_bits = 0, best_unit = 0, best_preamble = 0;
    unsigned best_repeats = 0;
    bool best_inverted = false;

    uint64_t run_value = 0;
    int run_bits = 0, run_unit = 0, run_end = -1;
    unsigned run_repeats = 0;

    for (int i = 0; i + 1 < edges; i++) {
        int unit = 0, units = 0;
        if (!preamble_at(pulse, i, edges, &unit, &units)) continue;

        uint64_t value = 0;
        bool inverted = false;
        const int bits = read_bits(pulse, i + 2, edges, unit, &value, &inverted);
        if (bits < SUBGHZ_PWM_MIN_BITS) continue;

        /* Back to back, same length, same payload, same timing: a repeat.
           Anything else starts a new run. */
        if (i == run_end && bits == run_bits && value == run_value &&
            near(unit, run_unit)) {
            run_repeats++;
        } else {
            run_value = value;
            run_bits = bits;
            run_unit = unit;
            run_repeats = 1;
        }
        run_end = i + 2 + bits * 2;

        if (run_repeats > best_repeats) {
            best_repeats = run_repeats;
            best_value = value;
            best_bits = bits;
            best_unit = unit;
            best_preamble = units;
            best_inverted = inverted;
        }
        i = run_end - 1;
    }

    if (best_repeats < 2) return false;

    out->value = best_value;
    out->bits = (uint8_t)best_bits;
    out->repeats = (uint8_t)(best_repeats > 255 ? 255 : best_repeats);
    out->unit_us = (uint16_t)best_unit;
    out->preamble_units = (uint16_t)best_preamble;
    out->inverted = best_inverted;

    /* Twenty-four bits in this family is EV1527 and its clones, where the
       payload splits into a 20-bit address fixed at the factory and a 4-bit
       button. Other lengths are reported as a payload, not guessed at. */
    if (best_bits == 24) {
        out->family = "EV1527";
        out->id = (uint32_t)(best_value >> 4) & 0xFFFFFu;
        out->button = (uint8_t)(best_value & 0xFu);
    }
    return true;
}

size_t subghz_pwm_format(const subghz_pwm_t *in, char *out, size_t n)
{
    if (!out || n == 0) return 0;
    if (!in || in->repeats == 0) { out[0] = 0; return 0; }

    int used;
    if (in->family)
        used = snprintf(out, n, "%s id %05lX btn %u",
                        in->family, (unsigned long)in->id, in->button);
    else
        used = snprintf(out, n, "%u bits %0*llX", in->bits,
                        (in->bits + 3) / 4, (unsigned long long)in->value);
    if (used < 0) { out[0] = 0; return 0; }
    return (size_t)used;
}
