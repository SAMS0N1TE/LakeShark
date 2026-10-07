#include "rs41_decode.h"
#include <math.h>
#include <string.h>

const uint8_t rs41_header[8] = {0x86,0x35,0xf4,0x40,0x93,0xdf,0x1a,0x60};
const uint8_t rs41_mask[64] = {
    0x96,0x83,0x3e,0x51,0xb1,0x49,0x08,0x98,0x32,0x05,0x59,0x0e,0xf9,0x44,0xc6,0x26,
    0x21,0x60,0xc2,0xea,0x79,0x5d,0x6d,0xa1,0x54,0x69,0x47,0x0c,0xdc,0xe8,0x5c,0xf1,
    0xf7,0x76,0x82,0x7f,0x07,0x99,0xa2,0x2c,0x93,0x7c,0x30,0x63,0xf5,0x10,0x2e,0x61,
    0xd0,0xbc,0xb4,0xb6,0x06,0xaa,0xf4,0x23,0x78,0x6e,0x3b,0xae,0xbf,0x7b,0x4c,0xc1
};
void rs41_whiten(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) p[i] ^= rs41_mask[i % 64];
}
uint16_t rs41_crc(const uint8_t *p, size_t n)
{
    uint16_t c = 0xffff;
    for (size_t i = 0; i < n; ++i) {
        c ^= (uint16_t)p[i] << 8;
        for (int j = 0; j < 8; ++j) c = (uint16_t)((c << 1) ^ ((c & 0x8000) ? 0x1021 : 0));
    }
    return c;
}
void rs41_init(rs41_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
    unsigned x = 1;
    for (int i = 0; i < 255; ++i) {
        d->exp[i] = (uint8_t)x; d->log[x] = (uint8_t)i;
        x <<= 1; if (x & 256) x ^= 0x11d;
    }
    for (int i = 255; i < 510; ++i) d->exp[i] = d->exp[i-255];
}
static uint8_t mul(const rs41_decoder_t *d, uint8_t a, uint8_t b)
{
    return a && b ? d->exp[d->log[a]+d->log[b]] : 0;
}
static uint8_t divgf(const rs41_decoder_t *d, uint8_t a, uint8_t b)
{
    return a ? d->exp[d->log[a]+255-d->log[b]] : 0;
}
static uint8_t evaluate(const rs41_decoder_t *d, const uint8_t *p, int degree, uint8_t x)
{
    uint8_t y = p[degree];
    while (degree) y = mul(d,y,x) ^ p[--degree];
    return y;
}
/* Ascending coefficients, roots alpha^0..alpha^23. Short frames have zero
   high coefficients. Berlekamp-Massey locates errors; a small Vandermonde
   solve gives their magnitudes without a second polynomial workspace. */
static int correct(rs41_decoder_t *d)
{
    bool clean = true;
    for (int i = 0; i < 24; ++i) {
        d->syndrome[i] = evaluate(d,d->cw,254,d->exp[i]);
        clean &= d->syndrome[i] == 0;
    }
    if (clean) return 0;
    memset(d->locator,0,25); memset(d->previous,0,25);
    d->locator[0] = d->previous[0] = 1;
    int degree = 0, shift = 1; uint8_t last = 1;
    for (int k = 0; k < 24; ++k) {
        uint8_t delta = d->syndrome[k];
        for (int j = 1; j <= degree; ++j) delta ^= mul(d,d->locator[j],d->syndrome[k-j]);
        if (!delta) { ++shift; continue; }
        memcpy(d->saved,d->locator,25);
        uint8_t scale = divgf(d,delta,last);
        for (int j = shift; j < 25; ++j) d->locator[j] ^= mul(d,scale,d->previous[j-shift]);
        if (2*degree <= k) {
            degree = k+1-degree; memcpy(d->previous,d->saved,25); last = delta; shift = 1;
        } else ++shift;
    }
    if (degree < 1 || degree > 12) return -1;
    int roots = 0;
    for (int i = 0; i < 255; ++i) {
        if (!evaluate(d,d->locator,degree,d->exp[(255-i)%255])) {
            if (roots == degree) return -1;
            d->positions[roots++] = (uint8_t)i;
        }
    }
    if (roots != degree) return -1;
    for (int row = 0; row < degree; ++row) {
        for (int col = 0; col < degree; ++col) d->matrix[row][col] = d->exp[(row*d->positions[col])%255];
        d->matrix[row][degree] = d->syndrome[row];
    }
    for (int col = 0; col < degree; ++col) {
        int pivot = col;
        while (pivot < degree && !d->matrix[pivot][col]) ++pivot;
        if (pivot == degree) return -1;
        for (int j = col; j <= degree; ++j) {
            uint8_t t = d->matrix[col][j]; d->matrix[col][j] = d->matrix[pivot][j]; d->matrix[pivot][j] = t;
        }
        uint8_t v = d->matrix[col][col];
        for (int j = col; j <= degree; ++j) d->matrix[col][j] = divgf(d,d->matrix[col][j],v);
        for (int row = 0; row < degree; ++row) if (row != col) {
            v = d->matrix[row][col];
            for (int j = col; j <= degree; ++j) d->matrix[row][j] ^= mul(d,v,d->matrix[col][j]);
        }
    }
    for (int i = 0; i < degree; ++i) d->cw[d->positions[i]] ^= d->matrix[i][degree];
    for (int i = 0; i < 24; ++i) if (evaluate(d,d->cw,254,d->exp[i])) return -1;
    return degree;
}
bool rs41_ecef(double x, double y, double z, double *lat, double *lon, double *alt)
{
    const double a = 6378137.0, e2 = 0.0066943799901413165, rad = 57.29577951308232;
    double p = hypot(x,y), r = hypot(p,z);
    if (!isfinite(r) || r < 6000000 || r > 6500000) return false;
    *lon = atan2(y,x)*rad;
    if (p < 1e-6) { *lat = z < 0 ? -90 : 90; *alt = fabs(z)-6356752.314245; return true; }
    double phi = atan2(z,p*(1-e2));
    for (int i = 0; i < 8; ++i) {
        double s = sin(phi), n = a/sqrt(1-e2*s*s);
        phi = atan2(z+e2*n*s,p);
    }
    double s = sin(phi), c = cos(phi), n = a/sqrt(1-e2*s*s);
    *lat = phi*rad; *alt = p*c+z*s-n*(1-e2*s*s);
    return *alt >= -1000 && *alt <= 100000;
}
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1]<<8); }
static uint32_t u32(const uint8_t *p) { return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static double signed32(const uint8_t *p) { uint32_t u = u32(p); return u & 0x80000000u ? (double)u-4294967296.0 : u; }
static double signed16(const uint8_t *p) { unsigned u = u16(p); return u & 0x8000 ? (double)u-65536 : u; }
bool rs41_decode(rs41_decoder_t *d, const uint8_t *wire, size_t n, rs41_report_t *r)
{
    if (!r) return false;
    memset(r,0,sizeof(*r));
    if (!d || !wire || (n != 320 && n != 518)) return false;
    memcpy(d->frame,wire,n); rs41_whiten(d->frame,n);
    if (memcmp(d->frame,rs41_header,8)) return false;
    for (int lane = 0; lane < 2; ++lane) {
        memset(d->cw,0,255);
        memcpy(d->cw,d->frame+8+24*lane,24);
        for (size_t i = 0; i < (n-56)/2; ++i) d->cw[24+i] = d->frame[56+2*i+lane];
        int fixed = correct(d);
        if (fixed < 0) return false;
        /* A correction in the absent suffix cannot belong to a short frame. */
        for (size_t i = 24+(n-56)/2; i < 255; ++i) if (d->cw[i]) return false;
        r->corrected += (unsigned)fixed;
        memcpy(d->frame+8+24*lane,d->cw,24);
        for (size_t i = 0; i < (n-56)/2; ++i) d->frame[56+2*i+lane] = d->cw[24+i];
    }
    if (d->frame[56] != (n == 320 ? 0x0f : 0xf0)) return false;
    /* Scan every block before exposing telemetry: SGM must never inherit a fix. */
    bool crypto = false;
    for (size_t at = 57; at+4 <= n;) {
        if (d->frame[at] == 0x80) crypto = true;
        size_t len = d->frame[at+1];
        if (at+len+4 > n) { ++r->bad_blocks; break; }
        const uint8_t *p = d->frame+at+2;
        if (rs41_crc(p,len) != u16(p+len)) ++r->bad_blocks;
        else { ++r->good_blocks; if (d->frame[at] == 0x80) r->encrypted = true; }
        at += len+4;
    }
    if (crypto) return r->encrypted;
    for (size_t at = 57; at+4 <= n;) {
        size_t len = d->frame[at+1];
        if (at+len+4 > n) break;
        const uint8_t *p = d->frame+at+2;
        if (rs41_crc(p,len) == u16(p+len)) switch (d->frame[at]) {
        case 0x79:
            if (len >= 12) {
                bool printable = true;
                for (int i = 0; i < 8; ++i) printable &= p[2+i] >= 33 && p[2+i] <= 126;
                if (printable) { memcpy(r->serial,p+2,8); r->frame = u16(p); r->battery = p[10]/10.0f; r->status = true; }
            }
            break;
        case 0x7a:
            if (len >= 36) { for (int i = 0; i < 12; ++i) r->ptu[i] = p[3*i] | (uint32_t)p[3*i+1]<<8 | (uint32_t)p[3*i+2]<<16; r->measurement = true; }
            break;
        case 0x7b:
            if (len >= 21 && p[18] >= 4 && rs41_ecef(signed32(p)/100,signed32(p+4)/100,signed32(p+8)/100,&r->lat,&r->lon,&r->alt)) {
                double phi = r->lat/57.29577951308232, lam = r->lon/57.29577951308232;
                r->climb = (float)((cos(phi)*cos(lam)*signed16(p+12)+cos(phi)*sin(lam)*signed16(p+14)+sin(phi)*signed16(p+16))/100);
                r->sats = p[18]; r->position = true;
            }
            break;
        case 0x7c:
            if (len >= 6) { r->week = u16(p); r->tow_ms = u32(p+2); r->gps_info = true; }
            break;
        }
        at += len+4;
    }
    return r->status || r->position || r->measurement || r->gps_info;
}
