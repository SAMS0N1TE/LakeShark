/* LS_TEST_SOURCES: ${APP}/subghz/subghz_pwm.c ${APP}/rec/rec_ook24.c
 *
 * Pulse-width OOK, the family every cheap remote, door contact and PIR
 * sensor on 433 uses. The board already pulled a 24-bit number off the air;
 * these cases are about reading any length, and about saying what the 24-bit
 * one means. */

#include "ls_test.h"

#include "subghz_pwm.h"
#include "rec_watch.h"

#include <string.h>

#define UNIT 350

/* One frame: a mark of `unit`, a space of `unit * preamble`, then each bit
   as short-mark/long-space for a zero and long-mark/short-space for a one.
   Written from the encoding rule rather than from a capture, so what it
   checks is that the decoder reads the rule the same way round. */
static int frame(int32_t *p, uint64_t value, int bits, int unit, int preamble)
{
    int n = 0;
    p[n++] = unit;
    p[n++] = -unit * preamble;
    for (int i = 0; i < bits; i++) {
        const int one = (int)((value >> (bits - 1 - i)) & 1u);
        p[n++] = one ? 3 * unit : unit;
        p[n++] = -(one ? unit : 3 * unit);
    }
    return n;
}

static int repeated(int32_t *p, uint64_t value, int bits, int times)
{
    int n = 0;
    for (int i = 0; i < times; i++)
        n += frame(p + n, value, bits, UNIT, 31);
    return n;
}

LS_CASE(a_twenty_four_bit_frame_is_named_and_split_into_address_and_button)
{
    /* The whole point: 0xA3B4C5 on the screen is a number, and
       "id 0A3B4C btn 5" is a door sensor with a button. */
    int32_t p[512];
    const int n = repeated(p, 0xA3B4C5u, 24, 3);

    subghz_pwm_t got;
    LS_CHECK_MSG(subghz_pwm_decode(p, n, &got), "a clean 24-bit frame did not decode");
    LS_EQ_UINT(got.bits, 24);
    LS_EQ_UINT(got.value, 0xA3B4C5u);
    LS_CHECK_MSG(got.family && !strcmp(got.family, "EV1527"),
                 "24 bits was not recognised as EV1527");
    LS_EQ_UINT(got.id, 0x0A3B4Cu);
    LS_EQ_UINT(got.button, 5);
    LS_CHECK(got.repeats >= 2);
    LS_NEAR(got.unit_us, UNIT, 1);

    char line[64];
    subghz_pwm_format(&got, line, sizeof(line));
    /* The address is 20 bits, so five hex digits, not six. */
    LS_CHECK_MSG(strstr(line, "EV1527") && strstr(line, "A3B4C"), line);
}

LS_CASE(the_lengths_the_old_decoder_could_not_read_now_decode)
{
    /* rec_decode_ook24 reads 24 bits and nothing else, so every other length
       in the same family came back as an unidentified capture. */
    static const struct { uint64_t value; int bits; } cases[] = {
        { 0xFFFu,               12 },
        { 0x12345678u,          32 },
        { 0x9ABCDEF012u,        40 },
        { 0x0123456789ABCDEFull, 64 },
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int32_t p[1024];
        const int n = repeated(p, cases[c].value, cases[c].bits, 3);

        subghz_pwm_t got;
        LS_CHECK_MSG(subghz_pwm_decode(p, n, &got),
                     "a %d-bit frame did not decode", cases[c].bits);
        LS_EQ_UINT(got.bits, (unsigned)cases[c].bits);
        LS_EQ_UINT(got.value, cases[c].value);
        /* Only 24 carries a layout this code is sure of. */
        if (cases[c].bits != 24)
            LS_CHECK_MSG(got.family == NULL,
                         "a %d-bit frame was given a device name",
                         cases[c].bits);
    }
}

LS_CASE(the_old_decoder_and_the_new_one_agree_on_the_frames_both_can_read)
{
    /* rec_decode_ook24 is what the sub-GHz screen has shipped on. Where both
       read a frame they must return the same payload, or one of them is
       reading the encoding backwards. */
    static const uint32_t values[] = { 0x000001u, 0xA3B4C5u, 0xFFFFFFu, 0x555555u };
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        int32_t p[1024];
        /* rec_decode_ook24 wants at least 100 edges before it will look. */
        const int n = repeated(p, values[i], 24, 6);

        rec_ook24_t old;
        subghz_pwm_t got;
        const bool old_ok = rec_decode_ook24(p, n, &old);
        const bool new_ok = subghz_pwm_decode(p, n, &got);
        LS_CHECK_MSG(old_ok, "the shipped decoder rejected %06lX",
                     (unsigned long)values[i]);
        LS_CHECK_MSG(new_ok, "the new decoder rejected %06lX",
                     (unsigned long)values[i]);
        if (!old_ok || !new_ok) continue;
        LS_EQ_UINT(got.value, old.value);
        LS_NEAR(got.unit_us, old.unit_us, 1);
    }
}

LS_CASE(one_frame_on_its_own_is_not_a_device)
{
    /* A single match is noise more often than a transmitter, which is the
       rule the shipped decoder already applies. */
    int32_t p[256];
    const int n = frame(p, 0xA3B4C5u, 24, UNIT, 31);

    subghz_pwm_t got;
    LS_CHECK_MSG(!subghz_pwm_decode(p, n, &got),
                 "an unrepeated frame was reported as a device");
}

LS_CASE(noise_does_not_decode)
{
    int32_t p[1024];
    ls_rng_t rng;
    ls_rng_seed(&rng, 4331527);
    for (int i = 0; i < 1024; i += 2) {
        p[i]     =  (int32_t)(ls_rng_u32(&rng) % 4000u) + 60;
        p[i + 1] = -(int32_t)(ls_rng_u32(&rng) % 4000u) - 60;
    }
    subghz_pwm_t got;
    LS_CHECK_MSG(!subghz_pwm_decode(p, 1024, &got), "noise decoded as a frame");
}

LS_CASE(timing_that_drifts_within_tolerance_still_decodes)
{
    /* Cheap transmitters drift and cheap receivers stretch edges. A decoder
       that only reads perfect timing reads nothing off real hardware. */
    int32_t p[1024];
    const int n = repeated(p, 0xA3B4C5u, 24, 4);
    for (int i = 0; i < n; i++)
        p[i] = p[i] > 0 ? p[i] * 112 / 100 : p[i] * 112 / 100;

    subghz_pwm_t got;
    LS_CHECK_MSG(subghz_pwm_decode(p, n, &got), "a 12%% stretch broke the decode");
    LS_EQ_UINT(got.value, 0xA3B4C5u);
}

LS_CASE(the_preamble_length_is_measured_not_assumed)
{
    /* EV1527 uses 31 units; PT2262 modules are commonly 23. Both are this
       family and the number is what tells them apart. */
    int32_t p[1024];
    int n = 0;
    for (int i = 0; i < 3; i++)
        n += frame(p + n, 0x0F0F0Fu, 24, UNIT, 23);

    subghz_pwm_t got;
    LS_CHECK_MSG(subghz_pwm_decode(p, n, &got), "a 23-unit preamble did not decode");
    LS_EQ_UINT(got.preamble_units, 23);
    LS_EQ_UINT(got.value, 0x0F0F0Fu);
}
