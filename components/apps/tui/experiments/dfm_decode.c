/*
 *  dfm09 (dfm06, dfm17)
 *  author: zilog80
 *
 *  Ported from rs1729/RS demod/mod/dfm09mod.c (https://github.com/rs1729/RS),
 *  GPL-3.0: the frame layout, deinterleaving, Hamming code, serial number
 *  assembly, field layout and temperature fit are that decoder's.
 */
/* See dfm_decode.h. The demodulation half of the upstream decoder is the
   LoRa chip's here; what is left is chips to telemetry. */
#include "dfm_decode.h"

#include <math.h>
#include <string.h>

#define B 8   /* codeword bits      */
#define S 4   /* data bits in one   */

/* Bit offsets of the blocks inside the 264 bits after the header. */
#define CONF 0
#define DAT1 56
#define DAT2 160
#define BODY_BITS (DFM_FRAME_BITS - DFM_HEAD_BITS)

#define SN_BIT 0x0100

/* A data block with more corrected codewords than this is not believed:
   the upstream decoder's threshold for what it reports. */
#define DAT_MAX_CORRECTED 4

/* Generator and parity check. A one-bit error in position j leaves the
   syndrome HE[j]; every column has odd weight, so two errors leave an even
   one that matches none and are caught. */
static const uint8_t G[8][4] = {
    { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 },
    { 0, 1, 1, 1 }, { 1, 0, 1, 1 }, { 1, 1, 0, 1 }, { 1, 1, 1, 0 },
};
static const uint8_t H[4][8] = {
    { 0, 1, 1, 1, 1, 0, 0, 0 },
    { 1, 0, 1, 1, 0, 1, 0, 0 },
    { 1, 1, 0, 1, 0, 0, 1, 0 },
    { 1, 1, 1, 0, 0, 0, 0, 1 },
};
static const uint8_t HE[8] = { 0x7, 0xB, 0xD, 0xE, 0x8, 0x4, 0x2, 0x1 };

static uint32_t bits2val(const uint8_t *bits, int len)
{
    uint32_t v = 0;
    for (int j = 0; j < len; j++) v = (v << 1) | (bits[j] & 1u);
    return v;
}

uint8_t dfm_hamming_encode(uint8_t nibble)
{
    uint8_t msg[4], cw = 0;
    for (int j = 0; j < 4; j++) msg[j] = (nibble >> (3 - j)) & 1;
    for (int i = 0; i < 8; i++) {
        uint8_t c = 0;
        for (int j = 0; j < 4; j++) c ^= G[i][j] & msg[j];
        cw = (uint8_t)((cw << 1) | c);
    }
    return cw;
}

static uint8_t syndrome(const uint8_t code[8])
{
    uint8_t s = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = 0;
        for (int j = 0; j < 8; j++) b ^= H[i][j] & code[j];
        s = (uint8_t)((s << 1) | b);
    }
    return s;
}

static int single_error(uint8_t syn)
{
    for (int j = 0; j < 8; j++)
        if (HE[j] == syn) return j;
    return -1;
}

/* Correct one codeword in place. Returns the bits changed, or -1 when it
   cannot be put right. `erased` marks bits whose chip pair was not
   Manchester: when the syndrome says two errors, a single erased bit is
   tried as one of them - one erasure and one error is still inside what a
   distance-4 code corrects. */
static int check(uint8_t code[8], const uint8_t erased[8])
{
    const uint8_t syn = syndrome(code);
    if (!syn) return 0;
    const int j = single_error(syn);
    if (j >= 0) { code[j] ^= 1; return 1; }
    for (int e = 0; e < 8; e++) {
        if (!erased[e]) continue;
        code[e] ^= 1;
        const uint8_t s2 = syndrome(code);
        if (!s2) return 1;
        const int k = single_error(s2);
        if (k >= 0 && k != e) { code[k] ^= 1; return 2; }
        code[e] ^= 1;
    }
    return -1;
}

/* Deinterleave L codewords from `str`, correct them and keep their data
   bits in `sym` (S*L). Returns the codewords corrected, or -1 when one could
   not be. Bits put right are added to *bits. */
static int hamming(const uint8_t *str, const uint8_t *era, int L, uint8_t *sym, int *bits)
{
    int fixed = 0;
    bool bad = false;
    for (int i = 0; i < L; i++) {
        uint8_t code[B], erased[B];
        for (int j = 0; j < B; j++) {
            code[j] = str[L * j + i];
            erased[j] = era[L * j + i];
        }
        const int r = check(code, erased);
        if (r < 0) bad = true;
        else if (r > 0) { fixed++; *bits += r; }
        for (int j = 0; j < S; j++) sym[S * i + j] = code[j];
    }
    return bad ? -1 : fixed;
}

/* ----------------------------------------------------- configuration -- */

static float fl24(uint32_t d)
{
    const int p = (d >> 20) & 0xF;
    return (float)(d & 0xFFFFF) / (float)(1u << p);
}

static void reset_cfgchk(dfm_t *d)
{
    memset(d->cfgchk24, 0, sizeof(d->cfgchk24));
    d->cfgchk = false;
    d->ptu_out = 0;
}

/* A type 0xA is a DFM-09 or, with a serial from 2023 on, a DFM-17; the
   upstream decoder also leans on the receiver's polarity, which is not known
   for this one. */
static bool dfm17_0xa(const dfm_t *d) { return d->sn >= 23000000u; }

static const char *type_name(const dfm_t *d)
{
    switch (d->sonde_typ & 0xF) {
    case 0x6: return "DFM06";
    case 0x7:
    case 0x8: return d->sn6 ? "DFM06P" : "PS15";
    case 0xA: return dfm17_0xa(d) ? "DFM17" : "DFM09";
    case 0xB: return "DFM17";
    case 0xC: return d->sensortyp == 'P' ? "DFM09P" : "DFM17";
    case 0xD: return "DFM17P";
    default:  return "DFM";
    }
}

static void conf_out(dfm_t *d, const uint8_t *cb, int ec)
{
    const uint8_t conf_id = (uint8_t)bits2val(cb, 4);

    if (conf_id > 4 && bits2val(cb + 8, 4 * 5) == 0) d->nul_ch = (uint8_t)bits2val(cb, 8);

    const bool dfm6typ = ((d->nul_ch & 0xF0) == 0x50) && (d->nul_ch & 0x0F);
    if (dfm6typ) d->ptu_out = 6;
    if (dfm6typ && (d->sonde_typ & 0xF) > 6) {
        d->sonde_typ = 0;
        d->max_ch = conf_id;
        reset_cfgchk(d);
    }

    if (conf_id > 5 && conf_id > d->max_ch && ec == 0 && bits2val(cb + 4, 4) == 0xC)
        d->max_ch = conf_id;

    /* The serial, in the last channel: at least six of them come first. */
    if (conf_id > 5 && (conf_id == (d->nul_ch >> 4) + 1 || conf_id == d->max_ch)) {
        const uint8_t sn2_ch = (uint8_t)bits2val(cb, 8);
        const uint8_t sn_ch = (sn2_ch >> 4) & 0xF;

        if ((d->nul_ch & 0x58) == 0x58) {
            /* DFM-06: 24 bits in one channel, believed when two agree. */
            const uint32_t sn6 = bits2val(cb + 4, 4 * 6);
            if (sn6 == d->sn6 && sn6 != 0) {
                d->sonde_typ = SN_BIT | sn_ch;
                d->ptu_out = 6;
                d->out.serial = sn6;
                d->out.serial_hex = true;
            } else {
                d->sonde_typ = 0;
                reset_cfgchk(d);
            }
            d->sn6 = sn6;
        } else if ((sn2_ch & 0xF) == 0xC || (sn2_ch & 0xF) == 0x0) {
            /* DFM-09/17: two 16-bit halves, the low nibble saying which. */
            const uint32_t val = bits2val(cb + 8, 4 * 5);
            const uint32_t hl = val & 0xF;
            if (hl < 2) {
                if (d->sn_ch != sn_ch) {
                    d->chx_bits = 0;
                    d->chx[0] = d->chx[1] = 0;
                    reset_cfgchk(d);
                }
                d->sn_ch = sn_ch;
                d->chx[hl] = (val >> 4) & 0xFFFF;
                d->chx_bits |= (uint8_t)(1u << hl);
                if (d->chx_bits == 3) {
                    const uint32_t sn = (d->chx[0] << 16) | d->chx[1];
                    if (sn == d->sn_x || d->sn_x == 0) {
                        d->sonde_typ = SN_BIT | sn_ch;
                        d->sn = sn;
                        d->ptu_out = (sn_ch >= 0xA && sn_ch <= 0xD) ? sn_ch : 0;
                        if (d->sn6 == 0 || (d->sonde_typ & 0xF) >= 0xA) {
                            d->out.serial = sn;
                            d->out.serial_hex = false;
                        }
                    } else {
                        d->sonde_typ = 0;
                        reset_cfgchk(d);
                    }
                    d->sn_x = sn;
                    d->chx_bits = 0;
                }
            }
        }
    }

    if (conf_id <= 8 && ec == 0) {
        d->cfgchk24[conf_id] = 1;
        d->meas24[conf_id] = fl24(bits2val(cb + 4, 4 * 6));
        bool ok = false;
        if (d->ptu_out >= 0x5) {
            ok = true;
            for (int i = 0; i <= 5; i++) ok = ok && d->cfgchk24[i];
        }
        if (d->ptu_out >= 0x7) ok = ok && d->cfgchk24[6] && d->cfgchk24[7];
        if (d->ptu_out >= 0x8) ok = ok && d->cfgchk24[8];
        d->cfgchk = ok;
    }

    d->sensortyp = 'T';
    d->rf = 220e3f;
    if (d->cfgchk) {
        if (d->ptu_out >= 0xD || (d->ptu_out >= 0xC && d->meas24[6] < 220e3f)) d->sensortyp = 'P';
        if (((d->ptu_out == 0xB || d->ptu_out == 0xC) && d->sensortyp == 'T') || d->ptu_out >= 0xD)
            d->rf = 332e3f;
        if (d->ptu_out == 0xA && d->sensortyp == 'T' && dfm17_0xa(d)) d->rf = 332e3f;
        if (d->ptu_out == 6 && (d->sonde_typ & 0xF) == 8) d->sensortyp = 'P';

        if (d->ptu_out >= 0xA) {
            /* The STM32 sondes: battery, the MCU's temperature, a counter. */
            const int ofs = d->sensortyp == 'P' ? 2 : 0;
            if (conf_id == 0x5 + ofs) d->status[0] = (float)bits2val(cb + 8, 4 * 4) / 1000.0f;
            if (conf_id == 0x6 + ofs) d->status[1] = (float)bits2val(cb + 8, 4 * 4) / 100.0f;
            if (conf_id == 0x7 + ofs && d->rf > 300e3f) d->status[2] = (float)bits2val(cb + 8, 4 * 4);
        } else {
            d->status[0] = d->status[1] = d->status[2] = 0;
        }
    }
    d->out.subtype = (uint8_t)(d->sonde_typ & 0xF);
    d->out.type = type_name(d);
}

/* NTC thermistor EPCOS B57540G0502, R25 = 5k: a Steinhart-Hart fit from
   -55 to +40 C over the reference resistors the sonde measures beside it. */
static float get_temp(const dfm_t *d)
{
    const float p0 = 1.09698417e-03f, p1 = 2.39564629e-04f,
                p2 = 2.48821437e-06f, p3 = 5.84354921e-08f;
    float f = d->meas24[0], f1 = d->meas24[3], f2 = d->meas24[4];
    if (d->sensortyp == 'P') {
        f  = d->meas24[1];
        f1 = d->meas24[5];
        f2 = d->meas24[6];
    }
    float t = 0;
    if (d->cfgchk && f2 > 0) {
        const float g = f2 / d->rf;
        float r = (f - f1) / g;
        if (f * f1 * f2 == 0) r = 0;
        if (r > 0) {
            const float l = logf(r);
            t = 1.0f / (p0 + p1 * l + p2 * l * l + p3 * l * l * l);
        }
    }
    return t - 273.15f;
}

/* -------------------------------------------------------------- data -- */

/* A cycle is complete when packets 0-4 and 8 arrived within six frames. */
static bool cycle_complete(const dfm_t *d)
{
    static const int NEED[] = { 0, 1, 2, 3, 4, 8 };
    for (unsigned i = 0; i < sizeof(NEED) / sizeof(NEED[0]); i++) {
        const uint32_t at = d->pck_at[NEED[i]];
        if (!at || d->frame - at >= 6) return false;
    }
    return true;
}

static void publish(dfm_t *d)
{
    dfm_report_t *o = &d->out;
    o->have_fix = true;
    o->fixes++;
    o->lat = d->lat;
    o->lon = d->lon;
    /* Mode 2 gives the height above the ellipsoid and, separately, how far
       the geoid sits from it; the later modes give it above sea level. */
    o->alt_m = (float)(d->posmode <= 2 ? d->alt + d->dmsl : d->alt);
    o->vel_h = d->vel_h;
    o->heading = d->dir;
    o->vel_v = d->vel_v;
    o->year = d->year;
    o->month = d->month;
    o->day = d->day;
    o->hour = d->hour;
    o->minute = d->minute;
    o->sec = d->sec;
    uint8_t sats = d->nsv;
    if (!sats)
        for (int j = 0; j < 32; j++) sats += (d->prn >> j) & 1;
    o->sats = sats;
    o->frnr = d->posmode <= 2 ? d->frnr : -1;
    o->have_batt = d->cfgchk && d->ptu_out >= 0xA && d->status[0] > 0;
    o->batt_v = d->status[0];
    o->have_temp = false;
    if (d->cfgchk && d->ptu_out) {
        const float t = get_temp(d);
        if (t > -270.0f) { o->have_temp = true; o->temp_c = t; }
    }
}

/* One data block; true when it completed a position. */
static bool dat_out(dfm_t *d, const uint8_t *db)
{
    const int fr_id = (int)bits2val(db + 48, 4);
    if (fr_id > 8) return false;
    d->pck_at[fr_id] = d->frame;

    if (fr_id == 0) {
        const int mode = (int)bits2val(db + 16, 8);
        d->posmode = (int8_t)(mode > 1 && mode < 5 ? mode : -1);
        d->frnr = (int)bits2val(db + 24, 8);
    }

    if (d->posmode <= 2) {
        switch (fr_id) {
        case 1:
            d->prn = bits2val(db, 32);
            d->sec = (float)bits2val(db + 32, 16) / 1000.0f;
            break;
        case 2:
            d->lat = (int32_t)bits2val(db, 32) / 1e7;
            d->vel_h = (int16_t)bits2val(db + 32, 16) / 1e2f;
            break;
        case 3:
            d->lon = (int32_t)bits2val(db, 32) / 1e7;
            d->dir = (float)(bits2val(db + 32, 16) & 0xFFFF) / 1e2f;
            break;
        case 4:
            d->alt = (int32_t)bits2val(db, 32) / 1e2;
            d->vel_v = (int16_t)bits2val(db + 32, 16) / 1e2f;
            break;
        case 5:
            d->dmsl = (int16_t)bits2val(db, 16) / 1e2f;
            break;
        default:
            break;
        }
    } else {
        /* Modes 3 and 4 (the DFM-17 among them): one packet earlier, and
           above sea level. Mode 3 repeats a second solution in 5-7, mode 4
           carries extra sensor data there; neither is needed here. */
        switch (fr_id) {
        case 0:
            d->sec = (float)bits2val(db, 16) / 1000.0f;
            d->vel_h = (int16_t)bits2val(db + 32, 16) / 1e2f;
            break;
        case 1:
            d->lat = (int32_t)bits2val(db, 32) / 1e7;
            d->dir = (float)(bits2val(db + 32, 16) & 0xFFFF) / 1e2f;
            break;
        case 2:
            d->lon = (int32_t)bits2val(db, 32) / 1e7;
            d->vel_v = (int16_t)bits2val(db + 32, 16) / 1e2f;
            break;
        case 3:
            d->alt = (int32_t)bits2val(db, 32) / 1e2;
            break;
        default:
            break;
        }
    }

    if (fr_id != 8) return false;
    d->year   = (uint16_t)bits2val(db, 12);
    d->month  = (uint8_t)bits2val(db + 12, 4);
    d->day    = (uint8_t)bits2val(db + 16, 5);
    d->hour   = (uint8_t)bits2val(db + 21, 5);
    d->minute = (uint8_t)bits2val(db + 26, 6);
    d->nsv    = (uint8_t)bits2val(db + 32, 8);

    const bool done = cycle_complete(d);
    if (done) publish(d);
    memset(d->pck_at, 0, sizeof(d->pck_at));
    return done;
}

/* ------------------------------------------------------------- frame -- */

void dfm_init(dfm_t *d)
{
    memset(d, 0, sizeof(*d));
    d->out.type = "DFM";
    d->out.frnr = -1;
    d->sensortyp = 'T';
    d->rf = 220e3f;
}

bool dfm_frame(dfm_t *d, const uint8_t *chips, bool inverted, dfm_frame_info_t *info)
{
    dfm_frame_info_t fi = { 0 };
    uint8_t bits[BODY_BITS], era[BODY_BITS];

    /* Manchester: 10 is a 0 and 01 a 1, so the bit is the second chip. A
       pair that is neither is kept as its second chip and marked. */
    const uint8_t flip = inverted ? 1 : 0;
    for (int k = 0; k < BODY_BITS; k++) {
        const int c = 2 * k;
        const uint8_t c1 = (uint8_t)(((chips[c >> 3] >> (7 - (c & 7))) & 1) ^ flip);
        const uint8_t c2 = (uint8_t)(((chips[(c + 1) >> 3] >> (7 - ((c + 1) & 7))) & 1) ^ flip);
        bits[k] = c2;
        era[k] = c1 == c2;
        fi.violations += era[k];
    }

    d->frame++;
    uint8_t conf[7 * S], dat1[13 * S], dat2[13 * S];
    const int r0 = hamming(bits + CONF, era + CONF, 7, conf, &fi.corrected);
    const int r1 = hamming(bits + DAT1, era + DAT1, 13, dat1, &fi.corrected);
    const int r2 = hamming(bits + DAT2, era + DAT2, 13, dat2, &fi.corrected);

    if (r0 >= 0) { fi.blocks_ok++; conf_out(d, conf, r0); }
    else fi.blocks_bad++;
    if (r1 >= 0) fi.blocks_ok++; else fi.blocks_bad++;
    if (r2 >= 0) fi.blocks_ok++; else fi.blocks_bad++;
    if (r1 >= 0 && r1 <= DAT_MAX_CORRECTED && dat_out(d, dat1)) fi.position = true;
    if (r2 >= 0 && r2 <= DAT_MAX_CORRECTED && dat_out(d, dat2)) fi.position = true;

    if (info) *info = fi;
    return fi.blocks_ok > 0;
}
