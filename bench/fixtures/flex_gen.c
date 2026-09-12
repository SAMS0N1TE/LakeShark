#include "flex_gen.h"
#include "ls_test.h"

#include <string.h>

#define DEMOD_RATE 32000
#define BCH_POLY   0x769u

/* Frame sync: A word (0xA6C6) followed by its complement (0x5939).  This is
   the pattern the whole receiver hunts for; every FLEX frame starts with it
   regardless of the payload rate/level. */
const uint32_t FLEX_SYNC_A = 0xA6C65939u;

/* Mode words - one per rate/level.  Each is B16 followed by ~B16, so a random
   32-bit word has essentially no chance of matching any of them by accident.
   The pairwise Hamming distances are all >= 14 out of 32, which is well clear
   of the 4-bit tolerance the classifier will accept. */
const uint32_t FLEX_MODE_WORDS[FLEX_MODE_COUNT] = {
    0x870CF8F3u,     /* FLEX_MODE_1600_2  =  0x870C | ~0x870C */
    0xB0684F97u,     /* FLEX_MODE_3200_2  =  0xB068 | ~0xB068 */
    0x3D9CC263u,     /* FLEX_MODE_3200_4  =  0x3D9C | ~0x3D9C */
    0x14D2EB2Du,     /* FLEX_MODE_6400_4  =  0x14D2 | ~0x14D2 */
};

static int popcount32(uint32_t v)
{
    int n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
}

int flex_mode_of(uint32_t word, int tol)
{
    int best = -1, best_d = 33;
    for (int m = 0; m < FLEX_MODE_COUNT; m++) {
        int d = popcount32(word ^ FLEX_MODE_WORDS[m]);
        if (d < best_d) { best_d = d; best = m; }
    }
    return (best_d <= tol) ? best : -1;
}

static const int MODE_BIT_RATE[FLEX_MODE_COUNT] = { 1600, 3200, 3200, 6400 };
static const int MODE_SYM_RATE[FLEX_MODE_COUNT] = { 1600, 3200, 1600, 3200 };
static const int MODE_LEVELS  [FLEX_MODE_COUNT] = { 2, 2, 4, 4 };

int flex_mode_bit_rate(flex_mode_t m)
{
    return (m >= 0 && m < FLEX_MODE_COUNT) ? MODE_BIT_RATE[m] : 0;
}
int flex_mode_sym_rate(flex_mode_t m)
{
    return (m >= 0 && m < FLEX_MODE_COUNT) ? MODE_SYM_RATE[m] : 0;
}
int flex_mode_levels(flex_mode_t m)
{
    return (m >= 0 && m < FLEX_MODE_COUNT) ? MODE_LEVELS[m] : 0;
}

void flex_tx_defaults(flex_tx_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode          = FLEX_MODE_1600_2;
    cfg->level         = 1.0f;
    cfg->preamble_bits = 64;
    cfg->seed          = 0xC0FFEEu;
}

/* BCH(31,21) + even parity, same generator polynomial as POCSAG.  21 data
   bits land at cw[30..10], the 10 BCH parity bits at cw[9..0], then the whole
   thing shifts up one for the even-parity bit at cw[0]. */
uint32_t flex_encode_cw(uint32_t data21)
{
    data21 &= 0x1FFFFFu;
    uint32_t reg = data21 << 10;
    for (int b = 30; b >= 10; b--)
        if (reg & (1u << b)) reg ^= (BCH_POLY << (b - 10));
    uint32_t cw31 = (data21 << 10) | (reg & 0x3FFu);
    uint32_t cw = cw31 << 1;
    cw |= (uint32_t)(popcount32(cw) & 1);
    return cw;
}

/* 8x32 block interleaver, column-first.  bit at row r, col c goes to
   transmission position c*8 + r.  This is what protects the block from a
   burst of consecutive bit errors - a burst of up to 8 bits corrupts at most
   1 bit per codeword, and BCH(31,21) can fix that in every one of them. */
void flex_interleave(const uint32_t *cw8, uint8_t *bits256)
{
    for (int c = 0; c < 32; c++)
        for (int r = 0; r < 8; r++)
            bits256[c * 8 + r] = (uint8_t)((cw8[r] >> (31 - c)) & 1);
}

void flex_deinterleave(const uint8_t *bits256, uint32_t *cw8)
{
    for (int r = 0; r < 8; r++) cw8[r] = 0;
    for (int c = 0; c < 32; c++)
        for (int r = 0; r < 8; r++)
            if (bits256[c * 8 + r]) cw8[r] |= (1u << (31 - c));
}

/* BIW: bit 20 = msg_type (0=alpha, 1=numeric), bits 19..13 = msg_len_cws. */
static uint32_t encode_biw(int msg_type, int msg_len_cws)
{
    uint32_t data21 = ((uint32_t)(msg_type & 1) << 20) |
                      (((uint32_t)msg_len_cws & 0x7Fu) << 13);
    return flex_encode_cw(data21);
}

/* Address: 21-bit RIC in the data field. */
static uint32_t encode_addr(uint32_t ric)
{
    return flex_encode_cw(ric & 0x1FFFFFu);
}

/* Three 7-bit chars per codeword: c0 in bits 20..14, c1 in 13..7, c2 in 6..0. */
static uint32_t encode_alpha_cw(int c0, int c1, int c2)
{
    uint32_t data21 = (((uint32_t)c0 & 0x7Fu) << 14) |
                      (((uint32_t)c1 & 0x7Fu) << 7)  |
                       ((uint32_t)c2 & 0x7Fu);
    return flex_encode_cw(data21);
}

/* Five 4-bit digits per codeword: d0 first at bits 19..16, d4 last at 3..0.
   Bit 20 unused for numeric. */
static uint32_t encode_numeric_cw(int d0, int d1, int d2, int d3, int d4)
{
    uint32_t data21 = (((uint32_t)d0 & 0xFu) << 16) |
                      (((uint32_t)d1 & 0xFu) << 12) |
                      (((uint32_t)d2 & 0xFu) << 8)  |
                      (((uint32_t)d3 & 0xFu) << 4)  |
                       ((uint32_t)d4 & 0xFu);
    return flex_encode_cw(data21);
}

/* FIW: cycle in bits 20..17, frame_no in 16..10.  Simplified content - the
   demodulator does not use these values, but the codeword must BCH-decode. */
static uint32_t encode_fiw(int cycle, int frame_no)
{
    uint32_t data21 = (((uint32_t)cycle & 0xFu) << 17) |
                      (((uint32_t)frame_no & 0x7Fu) << 10);
    return flex_encode_cw(data21);
}

static void push_word(uint8_t *bits, size_t *n, size_t cap, uint32_t w)
{
    for (int b = 31; b >= 0 && *n < cap; b--)
        bits[(*n)++] = (uint8_t)((w >> b) & 1u);
}

/* Bit array for one FLEX frame:
     preamble | sync A | mode word | FIW | 256-bit interleaved payload block */
static size_t build_frame_bits(const flex_tx_cfg_t *cfg,
                               uint32_t address, flex_msg_type_t msg_type,
                               const char *text,
                               uint8_t *bits, size_t bits_cap)
{
    size_t n = 0;

    if (flex_mode_levels(cfg->mode) == 4) {
        for (int i = 0; i < cfg->preamble_bits && n < bits_cap; i++) {
            int phase = i & 3;
            bits[n++] = (uint8_t)((phase == 2) ? 1 : 0);
        }
    } else {
        for (int i = 0; i < cfg->preamble_bits && n < bits_cap; i++)
            bits[n++] = (uint8_t)(1 - (i & 1));
    }

    push_word(bits, &n, bits_cap, FLEX_SYNC_A);
    push_word(bits, &n, bits_cap, FLEX_MODE_WORDS[cfg->mode]);
    push_word(bits, &n, bits_cap, encode_fiw(0, 0));

    /* Six message codewords available - CW 0 is BIW, CW 1 is address. */
    uint32_t msg_cws[6];
    for (int i = 0; i < 6; i++) msg_cws[i] = flex_encode_cw(0);
    int msg_len_cws = 0;

    if (text && *text) {
        if (msg_type == FLEX_MSG_ALPHA) {
            int len = 0; while (text[len]) len++;
            int idx = 0;
            for (int i = 0; i < len && idx < 6; i += 3) {
                int c0 = text[i] & 0x7F;
                int c1 = (i + 1 < len) ? (text[i + 1] & 0x7F) : 0;
                int c2 = (i + 2 < len) ? (text[i + 2] & 0x7F) : 0;
                msg_cws[idx++] = encode_alpha_cw(c0, c1, c2);
            }
            msg_len_cws = idx;
        } else {
            int digits[64];
            int nd = 0;
            for (const char *p = text; *p && nd < 64; p++)
                if (*p >= '0' && *p <= '9') digits[nd++] = *p - '0';
            int idx = 0;
            for (int i = 0; i < nd && idx < 6; i += 5) {
                int d[5] = {0,0,0,0,0};
                for (int k = 0; k < 5 && i + k < nd; k++) d[k] = digits[i + k];
                msg_cws[idx++] = encode_numeric_cw(d[0], d[1], d[2], d[3], d[4]);
            }
            msg_len_cws = idx;
        }
    }

    uint32_t cws[8];
    cws[0] = encode_biw((int)msg_type, msg_len_cws);
    cws[1] = encode_addr(address);
    for (int i = 0; i < 6; i++) cws[2 + i] = msg_cws[i];

    uint8_t block_bits[256];
    flex_interleave(cws, block_bits);
    for (int i = 0; i < 256 && n < bits_cap; i++) bits[n++] = block_bits[i];

    /* Trailing dotting - a few extra bits so the receiver's timing loop
       samples the last payload bit even after +/-500 ppm drift has moved
       the frame boundary a couple of tenths of a bit off nominal. */
    for (int i = 0; i < 16 && n < bits_cap; i++) bits[n++] = (uint8_t)(i & 1);

    return n;
}

/* Render a bit stream as a 2-FSK discriminator waveform at DEMOD_RATE.  Each
   bit becomes `sps` samples at (level or -level) + dc, with optional additive
   noise.  Symbol rate = baud (2-FSK has one bit per symbol). */
static size_t render_2fsk(const flex_tx_cfg_t *cfg, const uint8_t *bits,
                          size_t n_bits, int baud, float *out, size_t out_cap)
{
    double sps = (double)DEMOD_RATE / ((double)baud *
                                       (1.0 + cfg->baud_err_ppm * 1e-6));

    if (!out) return (size_t)(sps * (double)n_bits) + 8;

    ls_rng_t rng;
    ls_rng_seed(&rng, cfg->seed);

    size_t n = 0;
    double t = 0.0;

    for (size_t i = 0; i < n_bits && n < out_cap; i++) {
        double end = (double)(i + 1) * sps;
        int bit = bits[i] ? 1 : 0;
        if (cfg->invert) bit ^= 1;
        float lvl = (bit ? cfg->level : -cfg->level) + cfg->dc;

        while (t < end && n < out_cap) {
            float s = lvl;
            if (cfg->noise > 0.0f) s += ls_rng_noise(&rng) * (cfg->noise / 0.29f);
            out[n++] = s;
            t += 1.0;
        }
    }
    return n;
}

/* Render a bit stream as a 4-FSK discriminator waveform.  Bits are grouped
   into symbols of 2 (MSB first).  The mapping is Gray-coded so an adjacent
   level slip corrupts only one bit:
       00 -> +1     01 -> +1/3     11 -> -1/3     10 -> -1
   Symbol rate = baud/2 (two bits per symbol at the same on-air rate). */
static size_t render_4fsk(const flex_tx_cfg_t *cfg, const uint8_t *bits,
                          size_t n_bits, int baud, float *out, size_t out_cap)
{
    /* One symbol carries 2 bits. */
    int sym_rate = baud / 2;
    double sps = (double)DEMOD_RATE / ((double)sym_rate *
                                       (1.0 + cfg->baud_err_ppm * 1e-6));

    /* Pad up to an even number of bits with a repeated last bit. */
    size_t n_sym = (n_bits + 1) / 2;

    if (!out) return (size_t)(sps * (double)n_sym) + 8;

    ls_rng_t rng;
    ls_rng_seed(&rng, cfg->seed);

    size_t n = 0;
    double t = 0.0;
    static const float LEVELS[4] = { 1.0f, 1.0f / 3.0f, -1.0f, -1.0f / 3.0f };
    /*        LEVELS index 0=00 -> +1
                            1=01 -> +1/3
                            2=10 -> -1
                            3=11 -> -1/3         (gray) */

    for (size_t i = 0; i < n_sym && n < out_cap; i++) {
        int msb = bits[2 * i]     ? 1 : 0;
        int lsb = (2 * i + 1 < n_bits && bits[2 * i + 1]) ? 1 : 0;
        if (cfg->invert) { msb ^= 1; lsb ^= 1; }
        int sym = (msb << 1) | lsb;
        float lvl = LEVELS[sym] * cfg->level + cfg->dc;

        double end = (double)(i + 1) * sps;
        while (t < end && n < out_cap) {
            float s = lvl;
            if (cfg->noise > 0.0f) s += ls_rng_noise(&rng) * (cfg->noise / 0.29f);
            out[n++] = s;
            t += 1.0;
        }
    }
    return n;
}

size_t flex_tx_page(const flex_tx_cfg_t *cfg, uint32_t address,
                    flex_msg_type_t msg_type, const char *text,
                    float *out, size_t out_cap)
{
    static uint8_t bits[1024];
    size_t nb = build_frame_bits(cfg, address, msg_type, text, bits, sizeof(bits));
    int levels = flex_mode_levels(cfg->mode);
    int baud   = flex_mode_bit_rate(cfg->mode);
    if (levels == 4) return render_4fsk(cfg, bits, nb, baud, out, out_cap);
    return render_2fsk(cfg, bits, nb, baud, out, out_cap);
}

size_t flex_tx_idle(const flex_tx_cfg_t *cfg, float *out, size_t out_cap)
{
    return flex_tx_page(cfg, 0, FLEX_MSG_ALPHA, NULL, out, out_cap);
}
