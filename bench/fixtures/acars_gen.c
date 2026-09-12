#include "acars_gen.h"
#include "ls_test.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void acars_tx_defaults(acars_tx_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->level             = 1.0f;
    cfg->preamble_bits     = 128;
    cfg->corrupt_parity_at = -1;
    cfg->seed              = 0xACA25u;
}

static int popcount8(uint8_t v)
{
    int n = 0; while (v) { v &= (uint8_t)(v - 1); n++; } return n;
}

uint8_t acars_odd_parity_bit(uint8_t c)
{
    /* Parity bit chosen so total 1-count over all 8 bits is odd. */
    return (uint8_t)(((popcount8((uint8_t)(c & 0x7Fu)) & 1) == 0) ? 1u : 0u);
}

uint8_t acars_apply_parity(uint8_t c)
{
    uint8_t p = acars_odd_parity_bit(c);
    return (uint8_t)((c & 0x7Fu) | (p << 7));
}

int acars_byte_parity_ok(uint8_t byte)
{
    return (popcount8(byte) & 1) == 1;
}

uint16_t acars_crc16(const uint8_t *data, size_t n)
{
    /* CRC-16-CCITT, poly 0x1021, initial 0xFFFF, no reflection, no final XOR.
       Applied to the low 7 bits of each byte - the parity bit is deliberately
       masked out so that a single-bit parity hit is caught by the parity check
       but does NOT force the whole message to be dropped by the CRC.  This
       matches the ARINC 618 spirit ("parity is per-char, BCS is over the
       character stream") and lets the framer surface a parity error count
       alongside a message that otherwise decoded cleanly. */
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(data[i] & 0x7Fu) << 8;
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t acars_build_frame(const acars_msg_t *m, uint8_t *bytes, size_t cap)
{
    /* Fixed header + variable text + ETX + BCS + DEL.  Content bytes carry
       odd parity in bit 7; BCS bytes do not. */
    size_t n = 0;

#define PUSH_PAR(b) do { if (n < cap) bytes[n++] = acars_apply_parity((uint8_t)(b)); } while (0)
#define PUSH_RAW(b) do { if (n < cap) bytes[n++] = (uint8_t)(b);                    } while (0)

    PUSH_PAR(ACARS_SYN);
    PUSH_PAR(ACARS_SYN);

    size_t crc_begin = n;                /* CRC covers SOH through ETX incl. */

    PUSH_PAR(ACARS_SOH);
    PUSH_PAR(m->mode ? m->mode : '2');
    for (int i = 0; i < 7; i++)
        PUSH_PAR(m->reg[i] ? m->reg[i] : '.');
    PUSH_PAR(m->tak ? m->tak : 0x15);    /* NAK */
    PUSH_PAR(m->label[0] ? m->label[0] : 'H');
    PUSH_PAR(m->label[1] ? m->label[1] : '1');
    PUSH_PAR(m->block_id ? m->block_id : '1');
    PUSH_PAR(ACARS_STX);

    if (m->text) {
        for (const char *p = m->text; *p && n < cap - 5; p++)
            PUSH_PAR(*p);
    }

    PUSH_PAR(ACARS_ETX);

    /* CRC over the parity-encoded bytes from SOH through ETX inclusive. */
    uint16_t crc = acars_crc16(&bytes[crc_begin], n - crc_begin);
    PUSH_RAW((crc >> 8) & 0xFFu);
    PUSH_RAW( crc       & 0xFFu);

    PUSH_PAR(ACARS_DEL);

#undef PUSH_PAR
#undef PUSH_RAW

    return n;
}

/* Convert a byte stream to a bit stream, LSB bit first within each byte.
   Every bit becomes one MSK bit at the wire. */
static size_t bytes_to_bits(const uint8_t *bytes, size_t n_bytes,
                            uint8_t *bits, size_t bits_cap)
{
    size_t n = 0;
    for (size_t i = 0; i < n_bytes; i++) {
        uint8_t b = bytes[i];
        for (int k = 0; k < 8 && n < bits_cap; k++)
            bits[n++] = (uint8_t)((b >> k) & 1u);
    }
    return n;
}

size_t acars_tx_bits(const acars_tx_cfg_t *cfg, const uint8_t *bits,
                     size_t n_bits, float *out, size_t out_cap)
{
    /* Fractional samples per bit, so we can absorb clock error into the
       generated waveform.  8 nominal, less/more with +/-ppm. */
    double sps = (double)ACARS_SAMP_RATE / ((double)ACARS_BAUD *
                                            (1.0 + cfg->baud_err_ppm * 1e-6));

    if (!out) return (size_t)(sps * (double)n_bits) + 8;

    ls_rng_t rng;
    ls_rng_seed(&rng, cfg->seed);

    size_t n = 0;
    double t = 0.0;                       /* fractional sample position */
    double phase = 0.0;                   /* radians, continuous across bits */
    double dphi_mark  = 2.0 * M_PI * (double)ACARS_TONE_MARK  /
                        (double)ACARS_SAMP_RATE;
    double dphi_space = 2.0 * M_PI * (double)ACARS_TONE_SPACE /
                        (double)ACARS_SAMP_RATE;

    for (size_t i = 0; i < n_bits && n < out_cap; i++) {
        double end = (double)(i + 1) * sps;
        int    bit = bits[i] ? 1 : 0;
        if (cfg->invert) bit ^= 1;
        double dphi = bit ? dphi_mark : dphi_space;

        while (t < end && n < out_cap) {
            float s = (float)(sin(phase) * (double)cfg->level) + cfg->dc;
            if (cfg->noise > 0.0f) s += ls_rng_noise(&rng) * (cfg->noise / 0.29f);
            out[n++] = s;
            phase += dphi;
            t += 1.0;
        }
        /* Wrap so double precision doesn't drift over a long message. */
        if (phase >  2.0 * M_PI) phase -= 2.0 * M_PI;
        if (phase < -2.0 * M_PI) phase += 2.0 * M_PI;
    }
    return n;
}

/* Bit array cap.  Preamble (up to ~256) + frame (up to ~250 bytes = 2000
   bits) + slack. */
#define ACARS_BITS_CAP  3072

size_t acars_tx_bytes(const acars_tx_cfg_t *cfg, const uint8_t *bytes,
                      size_t n_bytes, float *out, size_t out_cap)
{
    static uint8_t bits[ACARS_BITS_CAP];
    size_t n_bits = 0;

    int pre = cfg->preamble_bits > 0 ? cfg->preamble_bits : 128;
    if (pre > (int)(ACARS_BITS_CAP - 8 * n_bytes)) pre = 32;

    for (int i = 0; i < pre && n_bits < ACARS_BITS_CAP; i++)
        bits[n_bits++] = (uint8_t)(1 - (i & 1));       /* 1010... bit sync */

    n_bits += bytes_to_bits(bytes, n_bytes,
                            bits + n_bits, ACARS_BITS_CAP - n_bits);

    return acars_tx_bits(cfg, bits, n_bits, out, out_cap);
}

size_t acars_tx_msg(const acars_tx_cfg_t *cfg, const acars_msg_t *m,
                    float *out, size_t out_cap)
{
    static uint8_t frame[512];
    size_t nb = acars_build_frame(m, frame, sizeof(frame));

    /* corrupt_parity_at names a character index counted from SOH inclusive
       (index 0 = SOH itself, 1 = mode, ..).  Flipping bit 7 inverts the
       parity and nothing else. */
    if (cfg->corrupt_parity_at >= 0) {
        size_t idx = 2 + (size_t)cfg->corrupt_parity_at;   /* skip two SYNs */
        if (idx < nb) frame[idx] ^= 0x80;
    }

    /* corrupt_text_bit flips one bit of the first text char AFTER the CRC
       has been computed - the frame is otherwise valid, but the BCS will
       no longer match the payload the receiver observes. */
    if (cfg->corrupt_text_bit) {
        /* Find STX, flip bit 0 of the byte right after it (first text char). */
        for (size_t i = 2; i + 1 < nb; i++) {
            if (acars_byte_parity_ok(frame[i]) &&
                (frame[i] & 0x7Fu) == ACARS_STX) {
                frame[i + 1] ^= 0x01;   /* changes payload without breaking parity */
                /* Re-apply parity to keep byte parity-valid, so the parity
                   check does not eat the corruption before the CRC does. */
                frame[i + 1] = acars_apply_parity(frame[i + 1]);
                break;
            }
        }
    }

    return acars_tx_bytes(cfg, frame, nb, out, out_cap);
}
