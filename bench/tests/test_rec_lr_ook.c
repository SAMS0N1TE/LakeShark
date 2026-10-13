/* LS_TEST_SOURCES: rec_lr_ook.c
 *
 * READ on the LR2021 slices on/off keying out of the chip's 125 kS/s I/Q
 * stream. These feed it synthetic stream blocks - noise, then a 24-bit
 * PT2262-style frame repeated - and check the pulse widths come back within
 * a sample, that a steady carrier is not taken for a remote, and that a
 * frame split across stream blocks is still one capture.
 */

#include "ls_test.h"
#include "rec_lr_ook.h"

#include <stdlib.h>
#include <string.h>

#define US 8
#define MAX_PAIRS 400000

static uint8_t s_iq[MAX_PAIRS * 4];
static int s_pairs;
static unsigned s_rng = 1;

static int noise(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (int)((s_rng >> 16) % 61) - 30;
}

static void put(int on, int us)
{
    for (int k = 0; k < us / US && s_pairs < MAX_PAIRS; k++, s_pairs++) {
        int i = noise() + (on ? 3000 : 0), q = noise();
        uint8_t *p = s_iq + s_pairs * 4;
        p[0] = (uint8_t)(i & 0xff); p[1] = (uint8_t)((i >> 8) & 0xff);
        p[2] = (uint8_t)(q & 0xff); p[3] = (uint8_t)((q >> 8) & 0xff);
    }
}

static void frame(uint32_t code)
{
    for (int b = 23; b >= 0; b--) {
        if ((code >> b) & 1) { put(1, 1050); put(0, 350); }
        else                 { put(1, 350);  put(0, 1050); }
    }
    put(1, 350); put(0, 10850);
}

static int s_caps, s_edges;
static int32_t s_edge[REC_LR_OOK_MAX_EDGES];

static void emit(void *ctx, const int32_t *edge, int edges, int peak)
{
    (void)ctx; (void)peak;
    s_caps++;
    s_edges = edges;
    memcpy(s_edge, edge, (size_t)edges * sizeof(edge[0]));
}

static rec_lr_ook_t s_ook;

static void run(size_t block)
{
    const rec_lr_ook_cfg_t cfg = { .min_pulse_us = 60, .gap_us = 30000, .min_edges = 6 };
    rec_lr_ook_init(&s_ook, &cfg);
    s_caps = s_edges = 0;
    const size_t bytes = (size_t)s_pairs * 4;
    for (size_t at = 0; at < bytes; at += block)
        rec_lr_ook_feed(&s_ook, s_iq + at, bytes - at < block ? bytes - at : block, emit, NULL);
}

LS_CASE(lr_ook_recovers_a_repeated_24_bit_frame)
{
    s_pairs = 0;
    put(0, 20000);
    for (int r = 0; r < 4; r++) frame(0xA53C19);
    put(0, 40000);
    run(1280);                                /* 320 pairs, as the stream gives */
    LS_EQ_INT(s_caps, 1);
    /* 4 frames x 25 highs, 2 edges each, the last low dropped. */
    LS_EQ_INT(s_edges, 4 * 50 - 1);
    /* First bit of A5 is 1: a long high, a short low. */
    LS_CHECK_MSG(abs(s_edge[0] - 1050) <= US, "first high %d us", (int)s_edge[0]);
    LS_CHECK_MSG(abs(s_edge[1] + 350) <= US, "first low %d us", (int)s_edge[1]);
    /* The second bit is 0: a short high, a long low. */
    LS_CHECK_MSG(abs(s_edge[2] - 350) <= US, "second high %d us", (int)s_edge[2]);
    LS_CHECK_MSG(abs(s_edge[3] + 1050) <= US, "second low %d us", (int)s_edge[3]);
}

LS_CASE(lr_ook_is_one_capture_whatever_the_block_size)
{
    s_pairs = 0;
    put(0, 20000);
    for (int r = 0; r < 2; r++) frame(0x123456);
    put(0, 40000);
    run(4);
    LS_EQ_INT(s_caps, 1);
    const int one_pair = s_edges;
    run(1276);
    LS_EQ_INT(s_caps, 1);
    LS_EQ_INT(s_edges, one_pair);
}

LS_CASE(lr_ook_ignores_a_steady_carrier_and_a_blip)
{
    s_pairs = 0;
    put(0, 20000);
    put(1, 200);
    put(0, 40000);                            /* two edges: a blip */
    put(1, 900000);                           /* a station that never keys */
    put(0, 40000);
    run(1280);
    LS_EQ_INT(s_caps, 0);
}

