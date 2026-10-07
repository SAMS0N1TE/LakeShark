#include "ls_test.h"
#include "fm_tone.h"
#include "scan_channels.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

static fm_tone_t detector;
static float block[1024];
static unsigned position;

static void feed(float hz, uint32_t word, int milliseconds, float deviation)
{
    unsigned remaining = (unsigned)milliseconds * 32;
    while (remaining) {
        unsigned n = remaining > 1024 ? 1024 : remaining;
        for (unsigned i = 0; i < n; ++i, ++position) {
            double t = position / 32000.0;
            /* Stronger speech above the sub-audible band and a DC offset. */
            float x = 0.08f + 0.13f * sin(6.283185307 * 710 * t) +
                      0.07f * sin(6.283185307 * 1237 * t);
            if (hz) x += deviation * sin(6.283185307 * hz * t + 0.7);
            if (word) {
                unsigned bit = (unsigned)floor(t * 134.4) % 23;
                x += (word & (1u << bit)) ? deviation : -deviation;
            }
            block[i] = x;
        }
        fm_tone_process(&detector, block, (int)n);
        remaining -= n;
    }
}

static void reset(void) { fm_tone_init(&detector); position = 0; }

LS_CASE(ctcss_standard_tones_under_voice)
{
    for (int i = 0; i < FM_CTCSS_COUNT; ++i) {
        reset();
        feed(fm_ctcss_tenths[i] * 0.1f, 0, 650, 0.025f);
        LS_EQ_INT(i + 1, detector.result.selection);
        LS_CHECK(detector.result.confidence > 0.65f);
    }
}

LS_CASE(ctcss_neighbours_and_qualification)
{
    reset(); feed(0, 0, 700, 0); feed(100, 0, 220, 0.025f);
    LS_EQ_INT(0, detector.result.selection);
    reset();
    feed(100, 0, 200, 0.025f);
    LS_EQ_INT(0, detector.result.selection);
    feed(100, 0, 200, 0.025f);
    LS_EQ_INT(13, detector.result.selection);
    reset(); feed(103.5f, 0, 650, 0.025f);
    LS_EQ_INT(14, detector.result.selection);
    reset(); feed(101.75f, 0, 650, 0.025f);
    LS_EQ_INT(0, detector.result.selection);
}

LS_CASE(all_dcs_codes_at_both_polarities)
{
    for (unsigned i = 0; i < FM_DCS_COUNT; ++i) {
        for (unsigned inv = 0; inv < 2; ++inv) {
            reset();
            position = 93; /* Acquisition begins between bit boundaries. */
            uint32_t word = fm_dcs_word(fm_dcs_codes[i]) ^ (inv ? 0x7fffff : 0);
            feed(0, word, 800, 0.04f);
            LS_CHECK(fm_tone_matches(51 + i + inv * FM_DCS_COUNT, detector.result));
        }
    }
}

LS_CASE(no_tone_and_short_dropout)
{
    reset(); feed(0, 0, 1000, 0);
    LS_EQ_INT(0, detector.result.selection);
    feed(100, 0, 650, 0.025f);
    feed(0, 0, 80, 0);
    LS_EQ_INT(13, detector.result.selection);
    feed(100, 0, 400, 0.025f);
    LS_EQ_INT(13, detector.result.selection);
    feed(0, 0, 1000, 0);
    LS_EQ_INT(0, detector.result.selection);
}

LS_CASE(dcs_golden_words_and_polarity)
{
    /* Fixed air words, independent of the detector's encoder. */
    const uint32_t words[] = {0x763813, 0x07b855, 0x20f9ec};
    const unsigned indices[] = {0, 20, 103};
    for (unsigned k = 0; k < 3; ++k) {
        LS_EQ_INT(words[k], fm_dcs_word(fm_dcs_codes[indices[k]]));
        for (int inv = 0; inv < 2; ++inv) {
            reset();
            uint32_t word = words[k] ^ (inv ? 0x7fffff : 0);
            feed(0, word, 200, 0.04f);
            LS_EQ_INT(0, detector.result.selection);
            feed(0, word, 600, 0.04f);
            unsigned expected = k == 2 ? 51 + 18 + (!inv) * FM_DCS_COUNT :
                                        51 + indices[k] + inv * FM_DCS_COUNT;
            LS_EQ_INT(expected, detector.result.selection);
            LS_CHECK(fm_tone_matches(51 + indices[k] + inv * FM_DCS_COUNT, detector.result));
            feed(0, 0, 80, 0);
            LS_CHECK(detector.result.selection != 0);
            feed(0, word, 600, 0.04f);
            LS_EQ_INT(expected, detector.result.selection);
            feed(0, 0, 800, 0);
            LS_EQ_INT(0, detector.result.selection);
        }
    }
}

LS_CASE(tone_gate_and_memory_encoding)
{
    fm_tone_result_t r = {13, 0.9f};
    LS_CHECK(fm_tone_matches(0, r));
    LS_CHECK(fm_tone_matches(13, r));
    LS_CHECK(!fm_tone_matches(14, r));
    LS_CHECK(!fm_tone_matches(13, (fm_tone_result_t){0}));
    for (unsigned v = 0; v < FM_TONE_CHOICES; ++v) {
        scan_channel_t c = {.rsv = (uint8_t)v,
                            .flags = v & 256 ? SCAN_FLAG_TONE_HIGH : 0};
        LS_EQ_INT(v, scan_channel_tone(&c));
    }
}

LS_CASE(carrier_gate_precedes_tone_gate)
{
    reset(); feed(100, 0, 650, 0.025f);
    LS_CHECK(fm_tone_receive(&detector, block, 0, true, 13));
    LS_CHECK(!fm_tone_receive(&detector, block, 0, true, 14));
    LS_CHECK(!fm_tone_receive(&detector, block, 1024, false, 0));
    LS_EQ_INT(0, detector.result.selection);
    LS_CHECK(!fm_tone_receive(&detector, block, 1024, true, 13));
    LS_CHECK(fm_tone_receive(&detector, block, 0, true, 0));
}

LS_CASE(tone_block_benchmark)
{
    reset();
    for (int i = 0; i < 1024; ++i) block[i] = 0.04f * sinf(i * 0.019635f);
    clock_t start = clock();
    for (int i = 0; i < 10000; ++i) fm_tone_process(&detector, block, 1024);
    double us = (double)(clock() - start) * 1e6 / CLOCKS_PER_SEC / 10000;
    printf("tone: %.1f us / 32 ms block; state %zu bytes\n", us, sizeof(detector));
    LS_CHECK(us < 3200); /* Less than 10 percent of a block on the host. */
}
