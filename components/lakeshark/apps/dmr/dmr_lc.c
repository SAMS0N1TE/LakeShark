#include "dmr.h"

#include <stdbool.h>
#include <string.h>

static uint32_t get_bits_msb(const uint8_t *buf, unsigned first, unsigned count)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned bit = first + i;
        v = (v << 1) | ((buf[bit >> 3] >> (7u - (bit & 7u))) & 1u);
    }
    return v;
}

void dmr_lc_parse(const uint8_t bits96[12], dmr_lc_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!bits96) return;
    out->protect_flag    = (uint8_t)get_bits_msb(bits96, 0, 1);
    /* bit 1 reserved, skipped */
    out->flco            = (uint8_t)get_bits_msb(bits96, 2, 6);
    out->fid             = (uint8_t)get_bits_msb(bits96, 8, 8);
    out->service_options = (uint8_t)get_bits_msb(bits96, 16, 8);
    out->destination     = get_bits_msb(bits96, 24, 24);
    out->source          = get_bits_msb(bits96, 48, 24);
}

/* Parity ordering, generator coefficients and burst masks independently
 * checked against MMDVM-Host RS129.cpp/DMRFullLC.cpp/DMRDefines.h, commit
 * 590c531391dfd3146073afbc3956f70d42c62a46 (Jonathan Naylor G4KLX and
 * upstream contributors, GPL-2.0-or-later). This implementation uses direct
 * GF(256) multiplication instead of upstream's logarithm lookup tables. */
static uint8_t gf_multiply(uint8_t a, uint8_t b)
{
    uint8_t result=0;
    while(b) {
        if(b&1u) result^=a;
        a=(uint8_t)((a<<1)^((a&0x80u)?0x1Du:0u));
        b>>=1;
    }
    return result;
}

int dmr_lc_decode(const uint8_t bits96[12], uint8_t data_type, dmr_lc_t *out)
{
    if(!bits96 || !out || (data_type!=1 && data_type!=2)) return 0;
    uint8_t parity[3]={0};
    for(unsigned i=0;i<9;i++) {
        uint8_t feedback=bits96[i]^parity[2];
        parity[2]=parity[1]^gf_multiply(14,feedback);
        parity[1]=parity[0]^gf_multiply(56,feedback);
        parity[0]=gf_multiply(64,feedback);
    }
    uint8_t mask=data_type==1?0x96u:0x99u;
    for(unsigned i=0;i<3;i++)
        if(bits96[9+i]!=(uint8_t)(parity[2-i]^mask)) return 0;
    dmr_lc_parse(bits96,out);
    return 1;
}

/* ---------------------------------------------------- RS(12,9) correct ---
 * The encoder above divides by g(x) = x^3 + 14x^2 + 56x + 64, whose roots
 * are a, a^2 and a^3 for a = 2 in GF(256) mod x^8+x^4+x^3+x^2+1: 2+4+8 = 14,
 * 8+16+32 = 56, 2*4*8 = 64. So the syndromes are the received word evaluated
 * at those three roots, octet 0 being the coefficient of x^11. One octet in
 * error at x^p with value e gives S1 = e a^p, S2 = e a^2p, S3 = e a^3p:
 * consistent only if S2^2 = S1 S3, located by a^p = S2/S1, valued by
 * e = S1^2/S2. Anything else is two or more octets and is refused. */
static uint8_t s_gf_exp[255];
static uint8_t s_gf_log[256];
static bool    s_gf_ready;

static void gf_init(void)
{
    if (s_gf_ready) return;
    uint8_t v = 1;
    for (unsigned i = 0; i < 255; i++) {
        s_gf_exp[i] = v;
        s_gf_log[v] = (uint8_t)i;
        v = gf_multiply(v, 2);
    }
    s_gf_ready = true;
}

static uint8_t gf_div(uint8_t a, uint8_t b)
{
    if (!a || !b) return 0;
    return s_gf_exp[(s_gf_log[a] + 255u - s_gf_log[b]) % 255u];
}

int dmr_rs129_correct(uint8_t cw[12])
{
    if (!cw) return -1;
    gf_init();
    uint8_t s[3];
    for (unsigned j = 0; j < 3; j++) {
        const uint8_t root = s_gf_exp[j + 1];
        uint8_t acc = 0;
        for (unsigned i = 0; i < 12; i++) acc = (uint8_t)(gf_multiply(acc, root) ^ cw[i]);
        s[j] = acc;
    }
    if (!s[0] && !s[1] && !s[2]) return 0;
    if (!s[0] || !s[1] || !s[2]) return -1;
    if (gf_multiply(s[1], s[1]) != gf_multiply(s[0], s[2])) return -1;
    const unsigned p = s_gf_log[gf_div(s[1], s[0])];
    if (p > 11) return -1;
    cw[11 - p] ^= gf_div(gf_multiply(s[0], s[0]), s[1]);
    return 1;
}

int dmr_lc_correct(const uint8_t bits96[12], uint8_t data_type, dmr_lc_t *out)
{
    if (!bits96 || !out || (data_type != 1 && data_type != 2)) return 0;
    if (dmr_lc_decode(bits96, data_type, out)) return 1;
    const uint8_t mask = data_type == 1 ? 0x96u : 0x99u;
    uint8_t cw[12];
    memcpy(cw, bits96, sizeof(cw));
    for (unsigned i = 9; i < 12; i++) cw[i] ^= mask;
    if (dmr_rs129_correct(cw) != 1) return 0;
    dmr_lc_parse(cw, out);
    return 2;
}

void dmr_lc_pack(const dmr_lc_t *lc, uint8_t out[DMR_LC_BYTES])
{
    if (!out) return;
    memset(out, 0, DMR_LC_BYTES);
    if (!lc) return;
    out[0] = (uint8_t)(((lc->protect_flag & 1u) << 7) | (lc->flco & 0x3Fu));
    out[1] = lc->fid;
    out[2] = lc->service_options;
    out[3] = (uint8_t)(lc->destination >> 16);
    out[4] = (uint8_t)(lc->destination >> 8);
    out[5] = (uint8_t)lc->destination;
    out[6] = (uint8_t)(lc->source >> 16);
    out[7] = (uint8_t)(lc->source >> 8);
    out[8] = (uint8_t)lc->source;
}

/* --------------------------------------------------------- GPS Info LC --- */

int dmr_gps_decode(const uint8_t lc9[DMR_LC_BYTES], dmr_gps_t *out)
{
    if (!lc9 || !out || (lc9[0] & 0x3Fu) != DMR_FLCO_GPS || lc9[1] != 0) return 0;
    /* After FLCO and FID: 4 reserved, 3 position error, 25 longitude, 24
       latitude - 56 bits, the rest of the LC. */
    uint32_t lon = get_bits_msb(lc9, 16 + 7, 25);
    uint32_t lat = get_bits_msb(lc9, 16 + 32, 24);
    int32_t slon = (lon & 0x1000000u) ? (int32_t)(lon | 0xFE000000u) : (int32_t)lon;
    int32_t slat = (lat & 0x800000u) ? (int32_t)(lat | 0xFF000000u) : (int32_t)lat;
    out->position_error = (uint8_t)get_bits_msb(lc9, 16 + 4, 3);
    out->longitude_raw  = slon;
    out->latitude_raw   = slat;
    out->longitude      = (float)slon * (360.0f / 33554432.0f);
    out->latitude       = (float)slat * (180.0f / 16777216.0f);
    return 1;
}

/* -------------------------------------------------------- Talker Alias --- */

void dmr_alias_reset(dmr_alias_t *a)
{
    if (a) memset(a, 0, sizeof(*a));
}

static unsigned utf8_put(char *out, unsigned at, unsigned cap, uint32_t cp)
{
    char tmp[3];
    unsigned n;
    if (cp < 0x80u)       { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800u) { tmp[0] = (char)(0xC0u | (cp >> 6));
                            tmp[1] = (char)(0x80u | (cp & 0x3Fu)); n = 2; }
    else                  { tmp[0] = (char)(0xE0u | (cp >> 12));
                            tmp[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                            tmp[2] = (char)(0x80u | (cp & 0x3Fu)); n = 3; }
    if (at + n >= cap) return at;
    memcpy(out + at, tmp, n);
    return at + n;
}

/* Bits of raw[] that have arrived, counting from the start, contiguous. */
static unsigned alias_bits_in(const dmr_alias_t *a)
{
    if (!(a->have & 1u)) return 0;
    unsigned bits = 49;
    for (unsigned b = 1; b <= 3 && (a->have & (1u << b)); b++) bits += 56;
    return bits;
}

static void alias_render(dmr_alias_t *a)
{
    const unsigned avail = alias_bits_in(a);
    const unsigned width = a->format == 0 ? 7u : a->format == 3 ? 16u : 8u;
    const unsigned first = a->format == 0 ? 0u : 1u;   /* octet aligned */
    unsigned at = 0, chars = 0;
    for (; chars < a->length; chars++) {
        const unsigned bit = first + chars * width;
        if (bit + width > avail) break;
        uint32_t cp = get_bits_msb(a->raw, bit, width);
        /* UTF-8 arrives as octets and passes through; a stray control or a
           surrogate half becomes '?' rather than reaching a terminal. */
        if (a->format == 2) {
            if (at + 1 >= sizeof(a->text)) break;
            a->text[at++] = cp < 0x20u ? '?' : (char)cp;
            continue;
        }
        if (cp < 0x20u || cp == 0x7Fu || (cp >= 0xD800u && cp < 0xE000u)) cp = '?';
        const unsigned next = utf8_put(a->text, at, sizeof(a->text), cp);
        if (next == at) break;
        at = next;
    }
    a->text[at] = '\0';
    a->complete = chars == a->length && a->length != 0;
}

bool dmr_alias_feed(dmr_alias_t *a, const uint8_t lc9[DMR_LC_BYTES])
{
    if (!a || !lc9 || lc9[1] != 0) return false;
    const uint8_t flco = lc9[0] & 0x3Fu;
    if (flco < DMR_FLCO_TA_HEADER || flco > DMR_FLCO_TA_BLOCK3) return false;

    if (flco == DMR_FLCO_TA_HEADER) {
        const uint8_t format = (uint8_t)get_bits_msb(lc9, 16, 2);
        const uint8_t length = (uint8_t)get_bits_msb(lc9, 18, 5);
        if ((a->have & 1u) && (format != a->format || length != a->length))
            dmr_alias_reset(a);
        a->format = format;
        a->length = length;
        for (unsigned i = 0; i < 49; i++) {
            const unsigned v = get_bits_msb(lc9, 23 + i, 1), d = i;
            if (v) a->raw[d >> 3] |= (uint8_t)(0x80u >> (d & 7u));
            else   a->raw[d >> 3] &= (uint8_t)~(0x80u >> (d & 7u));
        }
        a->have |= 1u;
    } else {
        const unsigned block = flco - DMR_FLCO_TA_HEADER;          /* 1..3 */
        const unsigned base = 49u + (block - 1u) * 56u;
        for (unsigned i = 0; i < 56; i++) {
            const unsigned v = get_bits_msb(lc9, 16 + i, 1), d = base + i;
            if (v) a->raw[d >> 3] |= (uint8_t)(0x80u >> (d & 7u));
            else   a->raw[d >> 3] &= (uint8_t)~(0x80u >> (d & 7u));
        }
        a->have |= (uint8_t)(1u << block);
    }
    alias_render(a);
    return true;
}
