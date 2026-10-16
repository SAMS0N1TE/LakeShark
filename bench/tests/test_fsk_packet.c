/* LS_TEST_SOURCES: fsk_packet.c, rec_fsk.c
 *
 * 2-FSK packet decoding from I/Q (the LR2021 recorder) and from REC's own
 * FSK slicer edges (the RTL path): preamble, sync word, payload and a CRC
 * found without being told the rate, the sync word or the length. Frames
 * are synthetic, shaped like CC1101 and SX1231 defaults. Set
 * LS_FSK_CAPTURE to a LakeShark .sub I/Q file, with LS_FSK_EXPECT holding
 * the payload hex, to check a real capture as well (kept out of the tree).
 */

#include "ls_test.h"
#include "fsk_packet.h"
#include "rec_fsk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 125000.0f
#define MAX_PAIRS 40000

static int16_t s_iq[2 * MAX_PAIRS];
static int32_t s_edges[4096];

static uint32_t s_seed;
static float noise1(void)
{
    /* sum of four uniforms: close enough to Gaussian for a noise floor */
    float a = 0;
    for (int i = 0; i < 4; i++) {
        s_seed = s_seed * 1664525u + 1013904223u;
        a += (float)(s_seed >> 8) / 16777216.0f - 0.5f;
    }
    return a * 1.7f;
}

typedef struct {
    float baud, dev_hz, offset_hz, amp, noise;
    bool invert;
} rf_t;

static int put_bits(uint8_t *bits, int n, uint32_t v, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) bits[n++] = (uint8_t)((v >> i) & 1u);
    return n;
}

/* preamble, sync (once or twice), body bytes */
static int frame(uint8_t *bits, int pre, uint16_t sync, int repeats, const uint8_t *body, int nbody)
{
    int n = 0;
    for (int i = 0; i < pre; i++) bits[n++] = (uint8_t)((i & 1) == 0);
    for (int r = 0; r < repeats; r++) n = put_bits(bits, n, sync, 16);
    for (int i = 0; i < nbody; i++) n = put_bits(bits, n, body[i], 8);
    return n;
}

/* 10 ms of noise, the burst with its frequency eased between tones over a
   quarter symbol (as a Gaussian filter does), then noise to the end. */
static size_t synth(const uint8_t *bits, int nbits, const rf_t *rf, size_t pairs)
{
    const float spb = FS / rf->baud;
    const size_t lead = (size_t)(0.010f * FS);
    const size_t body = (size_t)(nbits * spb);
    const int ease = (int)(spb / 4) > 1 ? (int)(spb / 4) : 1;
    float phase = 0, f_now = 0;
    for (size_t k = 0; k < pairs; k++) {
        float i = 0, q = 0;
        if (k >= lead && k < lead + body) {
            const int b = (int)((k - lead) / spb);
            const int bit = bits[b] ^ (rf->invert ? 1 : 0);
            const float want = rf->offset_hz + (bit ? rf->dev_hz : -rf->dev_hz);
            if (k == lead) f_now = want;
            f_now += (want - f_now) / (float)ease;
            phase += 2 * 3.14159265f * f_now / FS;
            i = rf->amp * cosf(phase);
            q = rf->amp * sinf(phase);
        }
        s_iq[2 * k] = (int16_t)lrintf(i + rf->noise * noise1());
        s_iq[2 * k + 1] = (int16_t)lrintf(q + rf->noise * noise1());
    }
    return pairs;
}

static int decode_iq(size_t pairs, fsk_pkt_t *out, int max, fsk_pkt_rf_t *rf)
{
    const size_t n = fsk_pkt_iq16_edges(s_iq, pairs, (uint32_t)FS, s_edges, 4096, rf);
    return n ? fsk_pkt_from_edges(s_edges, n, out, max) : 0;
}

static void with_crc(uint8_t *body, int n, uint16_t poly, uint16_t init, uint16_t xorout)
{
    const uint16_t c = fsk_pkt_crc16(body, (size_t)n, poly, init) ^ xorout;
    body[n] = (uint8_t)(c >> 8);
    body[n + 1] = (uint8_t)c;
}

LS_CASE(cc1101_fixed_length_2400)
{
    uint8_t body[8] = { 0x5A, 0x13, 0xC7, 0x81 };
    with_crc(body, 4, 0x8005, 0xFFFF, 0);
    uint8_t bits[512];
    const int nb = frame(bits, 64, 0xD391, 2, body, 6);
    const rf_t rf = { 2400, 4750, 8700, 300, 20, false };
    s_seed = 1;
    const size_t pairs = synth(bits, nb, &rf, 32767);
    fsk_pkt_t p[4];
    fsk_pkt_rf_t m;
    LS_EQ_INT(decode_iq(pairs, p, 4, &m), 1);
    LS_EQ_UINT(p[0].baud_std, 2400);
    LS_EQ_UINT(p[0].sync, 0xD391);
    LS_EQ_INT(p[0].sync_repeats, 2);
    LS_CHECK(p[0].sync_known);
    LS_EQ_INT(p[0].crc_kind, FSK_CRC_CC1101);
    LS_EQ_INT(p[0].len, 4);
    LS_CHECK(!memcmp(p[0].data, body, 4));
    LS_CHECK(!p[0].whitened && !p[0].inverted);
    LS_NEAR(m.offset_hz, 8700, 600);
    LS_NEAR(m.deviation_hz, 4750, 900);
    char line[128], want[128];
    fsk_pkt_describe(&p[0], line, sizeof line);
    snprintf(want, sizeof want, "2-FSK 2400 bd sync D391x2 4 B 5A 13 C7 81 CRC %02X%02X ok", body[4], body[5]);
    LS_EQ_STR(line, want);
}

LS_CASE(length_byte_pn9_inverted_4800)
{
    uint8_t clear[16] = { 0x05, 0x10, 0x20, 0x30, 0x40, 0x50 };
    with_crc(clear, 6, 0x8005, 0xFFFF, 0);
    uint8_t sent[16];
    memcpy(sent, clear, 8);
    fsk_pkt_pn9(sent, 8);
    uint8_t bits[512];
    const int nb = frame(bits, 32, 0xD391, 2, sent, 8);
    const rf_t rf = { 4800, 12000, -15000, 400, 25, true };
    s_seed = 2;
    const size_t pairs = synth(bits, nb, &rf, 20000);
    fsk_pkt_t p[4];
    LS_EQ_INT(decode_iq(pairs, p, 4, NULL), 1);
    LS_EQ_UINT(p[0].baud_std, 4800);
    LS_CHECK(p[0].inverted);
    LS_CHECK(p[0].whitened);
    LS_CHECK(p[0].length_byte);
    LS_EQ_INT(p[0].len, 6);
    LS_CHECK(!memcmp(p[0].data, clear, 6));
}

LS_CASE(sx1231_sync_ccitt_9600)
{
    uint8_t body[12] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03 };
    with_crc(body, 7, 0x1021, 0xFFFF, 0);
    uint8_t bits[512];
    const int nb = frame(bits, 40, 0x2DD4, 1, body, 9);
    const rf_t rf = { 9600, 20000, 3000, 500, 20, false };
    s_seed = 3;
    const size_t pairs = synth(bits, nb, &rf, 12000);
    fsk_pkt_t p[4];
    LS_EQ_INT(decode_iq(pairs, p, 4, NULL), 1);
    LS_EQ_UINT(p[0].baud_std, 9600);
    LS_EQ_UINT(p[0].sync, 0x2DD4);
    LS_EQ_INT(p[0].crc_kind, FSK_CRC_CCITT);
    LS_EQ_INT(p[0].len, 7);
    LS_CHECK(!memcmp(p[0].data, body, 7));
}

LS_CASE(unknown_sync_still_found_by_crc)
{
    uint8_t body[8] = { 0x42, 0x00, 0x99 };
    with_crc(body, 3, 0x8005, 0xFFFF, 0);
    uint8_t bits[512];
    const int nb = frame(bits, 48, 0xB34D, 1, body, 5);
    const rf_t rf = { 2400, 6000, 0, 300, 15, false };
    s_seed = 4;
    const size_t pairs = synth(bits, nb, &rf, 20000);
    fsk_pkt_t p[4];
    LS_EQ_INT(decode_iq(pairs, p, 4, NULL), 1);
    LS_CHECK(!p[0].sync_known);
    LS_EQ_UINT(p[0].sync, 0xB34D);
    LS_EQ_INT(p[0].crc_kind, FSK_CRC_CC1101);
    LS_EQ_INT(p[0].len, 3);
    LS_CHECK(!memcmp(p[0].data, body, 3));
}

LS_CASE(noise_alone_is_nothing)
{
    const rf_t rf = { 2400, 4750, 0, 0, 30, false };
    uint8_t bits[1] = {0};
    s_seed = 5;
    const size_t pairs = synth(bits, 0, &rf, 32767);
    fsk_pkt_t p[4];
    LS_EQ_INT(decode_iq(pairs, p, 4, NULL), 0);
}

LS_CASE(two_bursts_two_packets_repeat_once)
{
    uint8_t a[8] = { 0x01, 0x02 }, b[8] = { 0x03, 0x04 };
    with_crc(a, 2, 0x8005, 0xFFFF, 0);
    with_crc(b, 2, 0x8005, 0xFFFF, 0);
    uint8_t bits[2048];
    int nb = frame(bits, 32, 0xD391, 1, a, 4);
    for (int i = 0; i < 150; i++) bits[nb++] = 2;          /* carrier off */
    nb += frame(bits + nb, 32, 0xD391, 1, b, 4);
    for (int i = 0; i < 150; i++) bits[nb++] = 2;
    nb += frame(bits + nb, 32, 0xD391, 1, a, 4);           /* a again */
    /* synth with gaps: bit value 2 means no carrier */
    const float spb = FS / 2400;
    const size_t lead = 1250;
    float phase = 0;
    s_seed = 6;
    size_t pairs = lead + (size_t)(nb * spb) + 1250;
    LS_CHECK(pairs <= MAX_PAIRS);
    if (pairs > MAX_PAIRS) return;
    for (size_t k = 0; k < pairs; k++) {
        float i = 0, q = 0;
        if (k >= lead && k < lead + (size_t)(nb * spb)) {
            const uint8_t v = bits[(int)((k - lead) / spb)];
            if (v != 2) {
                phase += 2 * 3.14159265f * (v ? 5000.0f : -5000.0f) / FS;
                i = 300 * cosf(phase); q = 300 * sinf(phase);
            }
        }
        s_iq[2 * k] = (int16_t)lrintf(i + 20 * noise1());
        s_iq[2 * k + 1] = (int16_t)lrintf(q + 20 * noise1());
    }
    fsk_pkt_t p[4];
    LS_EQ_INT(decode_iq(pairs, p, 4, NULL), 2);
    LS_EQ_UINT(p[0].data[0], 0x01);
    LS_EQ_UINT(p[1].data[0], 0x03);
}

/* The RTL path: REC's own slicer at 256 kS/s, a trap-like transmitter
   (2400 baud, 19 kHz deviation, D391D391), edges straight into the packet
   stage as a standard build's REC capture holds them. */
LS_CASE(rec_slicer_edges_decode)
{
    uint8_t body[8] = { 0x5A, 0x13, 0xC7, 0x00, 0xFF, 0x81 };
    with_crc(body, 6, 0x8005, 0xFFFF, 0);
    uint8_t bits[512];
    const int nb = frame(bits, 64, 0xD391, 2, body, 8);
    const float fs = 256000, spb = fs / 2400;
    rec_fsk_t s;
    rec_fsk_init(&s, fs);
    float phase = 0;
    int n = 0, level = 0;
    uint32_t run = 0;
    s_seed = 7;
    const int lead = 2000, total = 2000 + (int)(nb * spb) + 2000;
    for (int k = 0; k < total; k++) {
        const bool carrier = k >= lead && k < lead + (int)(nb * spb);
        float i = 0, q = 0;
        if (carrier) {
            const int b = (int)((k - lead) / spb);
            phase += 2 * 3.14159265f * (3000.0f + (bits[b] ? 19000.0f : -19000.0f)) / fs;
            i = 60 * cosf(phase); q = 60 * sinf(phase);
        }
        const bool hi = rec_fsk_step(&s, (int)lrintf(i + 3 * noise1()), (int)lrintf(q + 3 * noise1()), carrier);
        const int lv = carrier ? (hi ? 1 : -1) : -1;
        if (!level) { if (carrier) { level = lv; run = 1; } continue; }
        if (lv == level) { run++; continue; }
        const int32_t us = (int32_t)lrintf(run * 1e6f / fs);
        s_edges[n++] = level > 0 ? us : -us;
        level = lv; run = 1;
    }
    s_edges[n++] = level > 0 ? (int32_t)(run * 1e6f / fs) : -(int32_t)(run * 1e6f / fs);
    fsk_pkt_t p[4];
    LS_EQ_INT(fsk_pkt_from_edges(s_edges, (size_t)n, p, 4), 1);
    LS_EQ_UINT(p[0].baud_std, 2400);
    LS_EQ_INT(p[0].crc_kind, FSK_CRC_CC1101);
    LS_EQ_INT(p[0].len, 6);
    LS_CHECK(!memcmp(p[0].data, body, 6));
}

LS_CASE(pn9_matches_cc1101_sequence)
{
    /* TI DN509: the first PN9 bytes for seed 0x1FF */
    uint8_t z[6] = {0};
    fsk_pkt_pn9(z, 6);
    LS_EQ_UINT(z[0], 0xFF);
    LS_EQ_UINT(z[1], 0xE1);
    LS_EQ_UINT(z[2], 0x1D);
    LS_EQ_UINT(z[3], 0x9A);
    LS_EQ_UINT(z[4], 0xED);
    LS_EQ_UINT(z[5], 0x85);
}

/* A real capture, when one is supplied. */
LS_CASE(real_capture_if_supplied)
{
    const char *path = getenv("LS_FSK_CAPTURE");
    const char *expect = getenv("LS_FSK_EXPECT");
    if (!path || !*path) { ls_note("LS_FSK_CAPTURE not set; skipped"); return; }
    FILE *f = fopen(path, "r");
    LS_CHECK_MSG(f != NULL, "cannot open %s", path);
    if (!f) return;
    static char line[256];
    size_t pairs = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "IQ_Data: ", 9)) continue;
        for (char *h = line + 9; h[0] && h[1] && h[0] != '\n' && pairs < MAX_PAIRS; h += 8) {
            unsigned b[4];
            if (sscanf(h, "%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3]) != 4) break;
            s_iq[2 * pairs] = (int16_t)(b[0] | b[1] << 8);
            s_iq[2 * pairs + 1] = (int16_t)(b[2] | b[3] << 8);
            pairs++;
        }
    }
    fclose(f);
    fsk_pkt_t p[4];
    fsk_pkt_rf_t m;
    const int got = decode_iq(pairs, p, 4, &m);
    LS_CHECK_MSG(got >= 1, "no packet in %s", path);
    if (got < 1) return;
    char desc[128];
    fsk_pkt_describe(&p[0], desc, sizeof desc);
    ls_note("%s | offset %.0f Hz dev %.0f Hz snr %.1f dB burst %.1f ms", desc, m.offset_hz, m.deviation_hz,
            m.snr_db, m.burst_ms);
    LS_CHECK(p[0].crc_kind != FSK_CRC_NONE);
    if (expect && *expect) {
        char hex[200] = "";
        size_t h = 0;
        for (int i = 0; i < p[0].len; i++) h += (size_t)snprintf(hex + h, sizeof hex - h, "%02X", p[0].data[i]);
        LS_EQ_STR(hex, expect);
    }
}
