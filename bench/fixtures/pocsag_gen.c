#include "pocsag_gen.h"
#include "ls_test.h"

#include <string.h>

#define FSC        0x7CD215D8u
#define IDLE       0x7A89C197u
#define BCH_POLY   0x769u
#define DEMOD_RATE 32000

void pocsag_tx_defaults(pocsag_tx_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->baud          = 1200;
    cfg->level         = 1.0f;
    cfg->noise         = 0.0f;
    cfg->dc            = 0.0f;
    cfg->baud_err_ppm  = 0.0f;
    cfg->invert        = 0;
    cfg->preamble_bits = 576;
    cfg->seed          = 0xC0FFEEu;
}

static int popcount32(uint32_t v)
{
    int n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
}

uint32_t pocsag_encode_raw(int flag, uint32_t data20)
{
    /* 21 data bits (flag + 20) at 30..10 of the 31-bit BCH word, remainder
       lands in 9..0, then the whole thing shifts up one for the parity bit. */
    uint32_t d21 = ((uint32_t)(flag & 1) << 20) | (data20 & 0xFFFFFu);
    uint32_t reg = d21 << 10;

    for (int b = 30; b >= 10; b--)
        if (reg & (1u << b)) reg ^= (BCH_POLY << (b - 10));

    uint32_t cw31 = (d21 << 10) | (reg & 0x3FFu);
    uint32_t cw   = cw31 << 1;
    cw |= (uint32_t)(popcount32(cw) & 1);      /* even parity over all 32 */
    return cw;
}

uint32_t pocsag_encode_address(uint32_t ric, int func)
{
    uint32_t addr18 = (ric >> 3) & 0x3FFFFu;
    uint32_t data20 = (addr18 << 2) | (uint32_t)(func & 3);
    return pocsag_encode_raw(0, data20);
}

uint32_t pocsag_encode_message(uint32_t data20)
{
    return pocsag_encode_raw(1, data20);
}

static void push_word(uint8_t *bits, size_t *n, size_t cap, uint32_t w)
{
    for (int b = 31; b >= 0 && *n < cap; b--)
        bits[(*n)++] = (uint8_t)((w >> b) & 1u);
}

static size_t build_bits_raw(uint32_t ric, int func,
                             const uint8_t *msg_bits, size_t n_msg_bits,
                             int preamble_bits, uint8_t *bits, size_t bits_cap)
{
    size_t n = 0;

    for (int i = 0; i < preamble_bits && n < bits_cap; i++)
        bits[n++] = (uint8_t)(1 - (i & 1));      /* 1010... */

    /* The frame an address sits in is fixed by the RIC's low three bits, so
       the codeword index is not free. */
    int addr_idx = (int)(ric & 7u) * 2;

    uint32_t words[16];
    for (int i = 0; i < 16; i++) words[i] = IDLE;

    words[addr_idx] = pocsag_encode_address(ric, func);

    size_t consumed = 0;
    for (int i = addr_idx + 1; i < 16 && consumed < n_msg_bits; i++) {
        uint32_t data20 = 0;
        for (int b = 0; b < 20; b++) {
            uint32_t bit = (consumed < n_msg_bits) ? msg_bits[consumed] : 0;
            data20 = (data20 << 1) | bit;        /* codeword bit 30 first */
            consumed++;
        }
        words[i] = pocsag_encode_message(data20);
    }

    push_word(bits, &n, bits_cap, FSC);
    for (int i = 0; i < 16; i++) push_word(bits, &n, bits_cap, words[i]);

    push_word(bits, &n, bits_cap, FSC);
    for (int i = 0; i < 16; i++) push_word(bits, &n, bits_cap, IDLE);

    return n;
}

size_t pocsag_tx_bits(const pocsag_tx_cfg_t *cfg, const uint8_t *bits,
                      size_t n_bits, float *out, size_t out_cap)
{
    double sps = (double)DEMOD_RATE / ((double)cfg->baud *
                                       (1.0 + cfg->baud_err_ppm * 1e-6));

    if (!out) return (size_t)(sps * (double)n_bits) + 8;

    ls_rng_t rng;
    ls_rng_seed(&rng, cfg->seed);

    size_t n = 0;
    double t = 0.0;                      /* fractional sample position */

    for (size_t i = 0; i < n_bits; i++) {
        double end = (double)(i + 1) * sps;
        int    bit = bits[i] ? 1 : 0;
        if (cfg->invert) bit ^= 1;
        float  lvl = (bit ? cfg->level : -cfg->level) + cfg->dc;

        while (t < end && n < out_cap) {
            float s = lvl;
            if (cfg->noise > 0.0f) s += ls_rng_noise(&rng) * (cfg->noise / 0.29f);
            out[n++] = s;
            t += 1.0;
        }
    }
    return n;
}

size_t pocsag_build_bits(uint32_t ric, int func, const char *text,
                         int preamble_bits, uint8_t *bits, size_t bits_cap)
{
    /* 7-bit ASCII, least significant bit first, is how an alphanumeric page
       packs into the 20 message bits of each codeword. */
    uint8_t msg[MSG_GEN_MAX];
    size_t  n_msg = 0;
    if (text) {
        for (const char *p = text; *p && n_msg + 7 <= MSG_GEN_MAX; p++)
            for (int k = 0; k < 7; k++)
                msg[n_msg++] = (uint8_t)((*p >> k) & 1);
    }
    return build_bits_raw(ric, func, msg, n_msg, preamble_bits, bits, bits_cap);
}

size_t pocsag_tx_page(const pocsag_tx_cfg_t *cfg, uint32_t ric, int func,
                      const char *text, float *out, size_t out_cap)
{
    static uint8_t bits[BITS_GEN_MAX];
    size_t n_bits = pocsag_build_bits(ric, func, text, cfg->preamble_bits,
                                      bits, BITS_GEN_MAX);
    return pocsag_tx_bits(cfg, bits, n_bits, out, out_cap);
}

size_t pocsag_tx_page_raw(const pocsag_tx_cfg_t *cfg, uint32_t ric, int func,
                          const uint8_t *msg_bits, size_t n_msg_bits,
                          float *out, size_t out_cap)
{
    static uint8_t bits[BITS_GEN_MAX];
    size_t n_bits = build_bits_raw(ric, func, msg_bits, n_msg_bits,
                                   cfg->preamble_bits, bits, BITS_GEN_MAX);
    return pocsag_tx_bits(cfg, bits, n_bits, out, out_cap);
}
