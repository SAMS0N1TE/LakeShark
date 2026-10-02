/* P25 SITE FINDER's pure half: see p25_sitefind.h. The BCH(63,16)
   generator is OP25's (gr-op25_repeater/lib/bch.cc, bchG; Copyright 2010,
   KA1RBI), and the frame layout around the NID is the one its
   p25_framer.cc reads. */
#include "p25_sitefind.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* bchG as a polynomial, coefficient of x^i in bit i: degree 47. */
#define BCH_G 0xCD930BDD3B2Bull

/* The sign bits of the NID for each single information bit, bit k being
   NAC/DUID bit k (DUID in 3..0). The projection is linear, so a NID's sign
   bits are the XOR of these over its set bits; the bench checks every entry
   against p25sf_nid_encode. */
static const uint32_t BASIS[16] = {
    0x00B51F51u, 0x01E76BBFu, 0x008D551Du, 0x0229BCC1u,
    0x0048DED4u, 0x04537982u, 0x0091BDA8u, 0x0813EC55u,
    0x00710FBEu, 0x1027D8AAu, 0x00E21F7Cu, 0x20FAAE05u,
    0x00235547u, 0x40A728E4u, 0x00F3B5DFu, 0x80A93A77u,
};

uint32_t p25sf_fs_signs(uint64_t fs)
{
    uint32_t s = 0;
    for (int t = 0; t < 24; t++) s = (s << 1) | (uint32_t)((fs >> (47 - 2 * t)) & 1u);
    return s;
}

uint64_t p25sf_nid_encode(uint16_t nac, uint8_t duid)
{
    const uint64_t m = ((uint64_t)(nac & 0xFFFu) << 4) | (duid & 0xFu);
    /* Systematic: the information in x^62..x^47, the remainder under it. */
    uint64_t r = m << 47;
    for (int b = 62; b >= 47; b--)
        if (r & (1ull << b)) r ^= BCH_G << (b - 47);
    const uint64_t cw = (m << 47) | r;
    const uint64_t parity = (duid == 5 || duid == 10) ? 1u : 0u;
    return (cw << 1) | parity;
}

uint32_t p25sf_nid_signs(uint64_t nid)
{
    uint32_t s = 0;
    for (int t = 0; t < 32; t++) s = (s << 1) | (uint32_t)((nid >> (63 - 2 * t)) & 1u);
    return s;
}

uint32_t p25sf_payload_signs(const uint8_t *payload, bool inverted)
{
    uint32_t s = 0;
    for (int k = 0; k < 33; k++) {
        if (k == P25SF_STATUS_INDEX) continue;
        s = (s << 1) | (uint32_t)((payload[k / 8] >> (7 - k % 8)) & 1u);
    }
    return inverted ? ~s : s;
}

static int bits32(uint32_t v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (int)((((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24);
}

static int lowest_bit(uint32_t v)
{
    int n = 0;
    while (!(v & 1u)) { v >>= 1; n++; }
    return n;
}

bool p25sf_nid_decode(uint32_t signs, p25sf_nid_t *out)
{
    /* Every NID in Gray-code order, one basis XOR apart. Once one is within
       P25SF_MAX_DIST it is the only one that close (the minimum distance is
       6), so the walk stops there. */
    uint32_t cur = 0, m = 0, best_m = 0;
    int best = bits32(signs);
    for (uint32_t i = 1; i < 65536u && best > P25SF_MAX_DIST; i++) {
        const int k = lowest_bit(i);
        cur ^= BASIS[k];
        m ^= 1u << k;
        const int d = bits32(cur ^ signs);
        if (d < best) { best = d; best_m = m; }
    }
    if (out) {
        out->nac = (uint16_t)(best_m >> 4);
        out->duid = (uint8_t)(best_m & 0xFu);
        out->dist = (uint8_t)best;
    }
    return best <= P25SF_MAX_DIST && p25sf_duid_valid((uint8_t)(best_m & 0xFu));
}

static const uint8_t DUIDS[7] = { 0x0, 0x3, 0x5, 0x7, 0xA, 0xC, 0xF };
static const char *const DUID_NAMES[7] = { "HDU", "TDU", "LDU1", "TSBK", "LDU2", "PDU", "TDULC" };

int p25sf_duid_slot(uint8_t duid)
{
    for (int i = 0; i < 7; i++) if (DUIDS[i] == duid) return i;
    return -1;
}

bool p25sf_duid_valid(uint8_t duid) { return p25sf_duid_slot(duid) >= 0; }

const char *p25sf_duid_name(uint8_t duid)
{
    const int i = p25sf_duid_slot(duid);
    return i < 0 ? "?" : DUID_NAMES[i];
}

/* ----------------------------------------------------------- profile -- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Three hex digits standing alone at s[0..2]. */
static int nac_at(const char *s, size_t n, bool need_digit)
{
    if (n < 3) return -1;
    int v = 0;
    bool digit = false;
    for (int i = 0; i < 3; i++) {
        const int h = hexval(s[i]);
        if (h < 0) return -1;
        if (s[i] >= '0' && s[i] <= '9') digit = true;
        v = v * 16 + h;
    }
    if (n > 3 && isalnum((unsigned char)s[3])) return -1;
    if (need_digit && !digit) return -1;
    return v;
}

/* "NAC 8A1" anywhere in the comment. */
static int nac_named(const char *s, size_t n)
{
    for (size_t i = 0; i + 4 < n; i++) {
        if (memcmp(s + i, "NAC", 3) || (i && isalnum((unsigned char)s[i - 1]))) continue;
        size_t j = i + 3;
        while (j < n && (s[j] == ' ' || s[j] == ':' || s[j] == '=')) j++;
        const int v = nac_at(s + j, n - j, false);
        if (v >= 0) return v;
    }
    return -1;
}

static void set_label(char *dst, const char *s, size_t n)
{
    while (n && (*s == ' ' || *s == '-')) { s++; n--; }
    size_t k = 0;
    /* Up to the next " --" or the end. */
    while (k < n && !(k + 2 < n && s[k] == ' ' && s[k + 1] == '-' && s[k + 2] == '-')) k++;
    while (k && s[k - 1] == ' ') k--;
    if (k >= P25SF_LABEL) k = P25SF_LABEL - 1;
    memcpy(dst, s, k);
    dst[k] = 0;
}

int p25sf_profile_parse(const char *text, size_t len, uint32_t lo_hz, uint32_t hi_hz,
                        p25sf_chan_t *out, int max)
{
    if (!text || !out || max <= 0) return 0;
    int n = 0;
    int nac = -1;
    char label[P25SF_LABEL] = "";
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n' && text[end]) end++;
        const char *s = text + pos;
        size_t l = end - pos;
        pos = end + 1;
        if (end < len && !text[end]) pos = len;
        while (l && (s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) l--;
        while (l && (*s == ' ' || *s == '\t')) { s++; l--; }
        if (!l) continue;

        if (*s == '#' || *s == ';') {
            s++; l--;
            while (l && *s == ' ') { s++; l--; }
            if (l >= 3 && !memcmp(s, "---", 3)) {
                /* A section: its name, and its NAC or none. */
                size_t k = 3;
                while (k < l && s[k] == '-') k++;
                if (k < l && s[k] == ' ' && k + 1 < l && s[k + 1] != '-') {
                    set_label(label, s + k, l - k);
                    nac = nac_named(s, l);
                }
                continue;
            }
            int v = nac_named(s, l);
            if (v >= 0) { nac = v; continue; }
            /* "# 8A1 Manchester": a bare NAC with at least one digit in it,
               so a word such as BAD is not read as one. */
            v = nac_at(s, l, true);
            if (v >= 0 && l > 4 && s[3] == ' ') {
                nac = v;
                set_label(label, s + 4, l - 4);
            }
            continue;
        }
        if (l <= 8 || memcmp(s, "control=", 8)) continue;
        uint64_t hz = 0;
        size_t k = 8;
        while (k < l && s[k] >= '0' && s[k] <= '9' && hz < 10000000000ull) hz = hz * 10 + (uint64_t)(s[k++] - '0');
        if (k == 8 || (k < l && s[k] != '|')) continue;
        if (hz < lo_hz || hz > hi_hz) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) if (out[i].hz == hz) dup = true;
        if (dup || n >= max) continue;
        out[n].hz = (uint32_t)hz;
        out[n].nac = (int16_t)nac;
        snprintf(out[n].label, sizeof(out[n].label), "%s", label);
        n++;
    }
    return n;
}
