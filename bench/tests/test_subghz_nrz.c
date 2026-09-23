/* LS_TEST_SOURCES: ${APP}/subghz/subghz_nrz.c
 *
 * Run-length OOK, against a real capture rather than a fixture written by
 * the same hand that wrote the decoder. The payload and its field layout
 * were confirmed independently: a set of 101 captures whose file names
 * record the id, channel, trap and command that were transmitted, and every
 * field the decoder reports matches the name of the file it came from. */

#include "ls_test.h"

#include "subghz_nrz.h"
#include "subghz_nrz_capture.h"

#include <string.h>

#define CH1_T1_EDGES ((int)(sizeof(SUBGHZ_NRZ_CH1_T1) / sizeof(int32_t)))

static void payload_hex(const subghz_nrz_t *in, char *out, size_t n)
{
    const int nibbles = in->n_bits / 4;
    size_t w = 0;
    for (int i = 0; i < nibbles && w + 2 < n; i++) {
        int v = 0;
        for (int b = 0; b < 4; b++) {
            const int idx = i * 4 + b;
            v = (v << 1) | ((in->bits[idx >> 3] >> (7 - (idx & 7))) & 1);
        }
        out[w++] = "0123456789ABCDEF"[v];
    }
    out[w] = 0;
}

LS_CASE(a_real_capture_decodes_to_the_frame_its_file_name_describes)
{
    subghz_nrz_t got;
    LS_CHECK_MSG(subghz_nrz_decode(SUBGHZ_NRZ_CH1_T1, CH1_T1_EDGES, &got),
                 "a real capture did not decode");
    if (!got.n_bits) return;

    /* 415 us was measured across the whole set; the device is consistent. */
    LS_CHECK_MSG(got.unit_us >= 395 && got.unit_us <= 435,
                 "unit came out at %u us, not about 415", got.unit_us);
    LS_CHECK_MSG(got.repeats >= 2, "only %u frames", got.repeats);

    char hex[80];
    payload_hex(&got, hex, sizeof(hex));
    /* id D391 twice, channel 01, trap 01, command 0001, then a checksum. */
    LS_CHECK_MSG(!strncmp(hex, "D391D391010100019435", 20),
                 "payload reads %s", hex);
}

LS_CASE(the_preamble_is_not_part_of_the_message)
{
    /* A run of ones keys the receiver and an alternating run trains its
       slicer. Leaving either in the payload buries the id under a constant
       that changes with how long the transmitter took to come up. */
    subghz_nrz_t got;
    LS_CHECK(subghz_nrz_decode(SUBGHZ_NRZ_CH1_T1, CH1_T1_EDGES, &got));
    LS_CHECK_MSG(got.training > 8,
                 "only %u preamble bits were stripped", got.training);

    char hex[80];
    payload_hex(&got, hex, sizeof(hex));
    LS_CHECK_MSG(hex[0] != '5' && hex[0] != 'F',
                 "the payload still opens on training: %s", hex);
}

LS_CASE(one_frame_without_a_repeat_is_not_a_message)
{
    /* Same rule the rest of the module applies: a single match is noise more
       often than a transmitter. */
    subghz_nrz_t got;
    LS_CHECK_MSG(!subghz_nrz_decode(SUBGHZ_NRZ_CH1_T1, 90, &got),
                 "a fragment too short to repeat was accepted");
}

LS_CASE(noise_does_not_decode)
{
    int32_t p[2048];
    ls_rng_t rng;
    ls_rng_seed(&rng, 415415);
    for (int i = 0; i < 2048; i += 2) {
        p[i]     =  (int32_t)(ls_rng_u32(&rng) % 3000u) + 40;
        p[i + 1] = -(int32_t)(ls_rng_u32(&rng) % 3000u) - 40;
    }
    subghz_nrz_t got;
    LS_CHECK_MSG(!subghz_nrz_decode(p, 2048, &got), "noise decoded as a frame");
}

LS_CASE(a_capture_with_no_carrier_at_all_is_refused)
{
    int32_t p[64];
    for (int i = 0; i < 64; i++) p[i] = (i & 1) ? -20 : 20;
    subghz_nrz_t got;
    LS_CHECK_MSG(!subghz_nrz_decode(p, 64, &got),
                 "runs far below any bit period were read as data");
}
