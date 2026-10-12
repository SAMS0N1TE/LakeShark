#include "ls_test.h"
#include "pager_iq.h"
#include "pocsag.h"
#include "pocsag_gen.h"
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, float sample)
{
    const uint16_t u = (uint16_t)(int16_t)lroundf(sample);
    p[0] = (uint8_t)u;
    p[1] = (uint8_t)(u >> 8);
}

/* Continuous phase FSK, quantized to signed IQ16, with channel offset,
   transmitter clock error and independent additive noise on I and Q. */
static uint8_t *make_iq(int baud, int inv, int offset, int ppm, size_t *bytes)
{
    uint8_t bits[BITS_GEN_MAX];
    const size_t nb = pocsag_build_bits(1234568, 3, "IQ TEST PAGE", 576, bits, sizeof(bits));
    const double step = (double)baud * (1.0 + ppm / 1000000.0) / PGR_IQ_INPUT_RATE;
    const size_t pairs = (size_t)ceil((nb + 8) / step);
    uint8_t *iq = malloc(pairs * 4);
    LS_CHECK(iq != NULL);
    ls_rng_t rng;
    ls_rng_seed(&rng, 0x12345678);
    double phase = 0.73;
    for (size_t k = 0; k < pairs; k++) {
        size_t at = (size_t)(0.37 + k * step);
        if (at >= nb) at = nb - 1;
        const int bit = bits[at] ^ inv;
        const double hz = offset + (bit ? 4500 : -4500);
        phase = remainder(phase + 6.283185307179586 * hz / PGR_IQ_INPUT_RATE,
                          6.283185307179586);
        put16(iq + k * 4, (float)(12000 * cos(phase)) + 2500 * ls_rng_noise(&rng));
        put16(iq + k * 4 + 2, (float)(12000 * sin(phase)) + 2500 * ls_rng_noise(&rng));
    }
    *bytes = pairs * 4;
    return iq;
}

static void feed(pgr_iq_t *s, pocsag_ctx_t *d, const uint8_t *iq, size_t bytes)
{
    static const size_t CHUNKS[] = { 1, 3, 19, 513, 2, 1023, 7, 4096 };
    float out[206];
    unsigned c = 0;
    for (size_t at = 0; at < bytes;) {
        size_t n = CHUNKS[c++ % (sizeof(CHUNKS) / sizeof(CHUNKS[0]))];
        if (n > bytes - at) n = bytes - at;
        size_t got = 0;
        LS_CHECK(pgr_iq_process(s, iq + at, n, out, 206, &got));
        pocsag_process(d, out, (int)got);
        at += n;
    }
}

LS_CASE(all_rates_polarities_offset_noise_and_chunk_seams)
{
    const int rates[] = {512, 1200, 2400};
    for (unsigned r = 0; r < 3; r++) for (int inv = 0; inv < 2; inv++) {
        fm_state_t *text = calloc(1, sizeof(*text));
        LS_CHECK(text != NULL);
        pocsag_ctx_t *d = pocsag_create(text, rates[r]);
        LS_CHECK(d != NULL);
        pocsag_sample_rate(d, PGR_IQ_OUTPUT_RATE);
        pgr_iq_t s;
        pgr_iq_init(&s);
        size_t bytes;
        uint8_t *iq = make_iq(rates[r], inv, inv ? -3000 : 3000, inv ? -800 : 800, &bytes);
        feed(&s, d, iq, bytes);
        LS_CHECK_MSG(pocsag_n_pages(d) == 1, "rate %d inv %d pages %u frames %u cwerr %u",
                     rates[r], inv, pocsag_n_pages(d), pocsag_n_frames(d), pocsag_n_cwerr(d));
        LS_EQ_UINT(pocsag_n_cwerr(d), 0);
        LS_EQ_UINT(text->pages[0].address, 1234568);
        LS_EQ_UINT(text->pages[0].function, 3);
        LS_EQ_STR(text->pages[0].text, "IQ TEST PAGE");
        LS_EQ_INT(pocsag_inverted(d), inv);
        /* Discontinuities reset both states, retaining decoder totals. */
        pgr_iq_reset(&s);
        pocsag_seam(d);
        feed(&s, d, iq, bytes);
        LS_EQ_UINT(pocsag_n_pages(d), 2);
        free(iq);
        pocsag_destroy(d);
        free(text);
    }
}

LS_CASE(chunking_is_sample_exact_and_capacity_failure_is_retryable)
{
    size_t bytes;
    uint8_t *iq = make_iq(2400, 0, 1000, 0, &bytes);
    const size_t cap = bytes / 20 + 1;
    float *whole = malloc(cap * sizeof(*whole));
    float *split = malloc(cap * sizeof(*split));
    LS_CHECK(whole && split);
    pgr_iq_t a, b;
    pgr_iq_init(&a);
    pgr_iq_init(&b);
    size_t na = 0, nb = 0, got = 99;
    LS_CHECK(!pgr_iq_process(&a, iq, bytes, whole, 0, &got));
    LS_EQ_UINT(got, 0);
    LS_CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    LS_CHECK(pgr_iq_process(&a, iq, bytes, whole, cap, &na));
    for (size_t k = 0; k < bytes;) {
        const size_t n = bytes - k > 137 ? 137 : bytes - k;
        LS_CHECK(pgr_iq_process(&b, iq + k, n, split + nb, cap - nb, &got));
        nb += got;
        k += n;
    }
    LS_EQ_UINT(na, bytes / 20);
    LS_EQ_UINT(nb, na);
    LS_CHECK(memcmp(whole, split, na * sizeof(*whole)) == 0);
    LS_CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    pgr_iq_reset(&b);
    pgr_iq_init(&a);
    LS_CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    free(whole); free(split); free(iq);
}

LS_CASE(zero_input_is_finite_and_safe)
{
    pgr_iq_t s;
    pgr_iq_init(&s);
    uint8_t zeros[1000] = {0};
    float out[50];
    size_t got;
    LS_CHECK(pgr_iq_process(&s, zeros, sizeof(zeros), out, 50, &got));
    LS_EQ_UINT(got, 50);
    for (size_t k = 0; k < got; k++) LS_CHECK(isfinite(out[k]) && out[k] == 0.0f);
    LS_CHECK(pgr_iq_process(&s, NULL, 0, NULL, 0, &got));
    LS_EQ_UINT(got, 0);
}
