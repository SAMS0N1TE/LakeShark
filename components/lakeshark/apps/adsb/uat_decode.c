/* Independent UAT RS implementation. Parameters checked against
   https://github.com/mutability/dump978/blob/master/fec.c (GPL-2.0-or-later).
   dump978 itself is GPL; its bundled Phil Karn libfec is LGPL (fec/README.fec).
   No upstream implementation is copied here. */
#include "uat_decode.h"
#include <string.h>
/* Generated from x^8+x^7+x^2+x+1. No runtime table setup. */
static const uint8_t gf_exp[510] = {
    1, 2, 4, 8, 16, 32, 64, 128, 135, 137, 149, 173, 221, 61, 122, 244,
    111, 222, 59, 118, 236, 95, 190, 251, 113, 226, 67, 134, 139, 145, 165, 205,
    29, 58, 116, 232, 87, 174, 219, 49, 98, 196, 15, 30, 60, 120, 240, 103,
    206, 27, 54, 108, 216, 55, 110, 220, 63, 126, 252, 127, 254, 123, 246, 107,
    214, 43, 86, 172, 223, 57, 114, 228, 79, 158, 187, 241, 101, 202, 19, 38,
    76, 152, 183, 233, 85, 170, 211, 33, 66, 132, 143, 153, 181, 237, 93, 186,
    243, 97, 194, 3, 6, 12, 24, 48, 96, 192, 7, 14, 28, 56, 112, 224,
    71, 142, 155, 177, 229, 77, 154, 179, 225, 69, 138, 147, 161, 197, 13, 26,
    52, 104, 208, 39, 78, 156, 191, 249, 117, 234, 83, 166, 203, 17, 34, 68,
    136, 151, 169, 213, 45, 90, 180, 239, 89, 178, 227, 65, 130, 131, 129, 133,
    141, 157, 189, 253, 125, 250, 115, 230, 75, 150, 171, 209, 37, 74, 148, 175,
    217, 53, 106, 212, 47, 94, 188, 255, 121, 242, 99, 198, 11, 22, 44, 88,
    176, 231, 73, 146, 163, 193, 5, 10, 20, 40, 80, 160, 199, 9, 18, 36,
    72, 144, 167, 201, 21, 42, 84, 168, 215, 41, 82, 164, 207, 25, 50, 100,
    200, 23, 46, 92, 184, 247, 105, 210, 35, 70, 140, 159, 185, 245, 109, 218,
    51, 102, 204, 31, 62, 124, 248, 119, 238, 91, 182, 235, 81, 162, 195, 1,
    2, 4, 8, 16, 32, 64, 128, 135, 137, 149, 173, 221, 61, 122, 244, 111,
    222, 59, 118, 236, 95, 190, 251, 113, 226, 67, 134, 139, 145, 165, 205, 29,
    58, 116, 232, 87, 174, 219, 49, 98, 196, 15, 30, 60, 120, 240, 103, 206,
    27, 54, 108, 216, 55, 110, 220, 63, 126, 252, 127, 254, 123, 246, 107, 214,
    43, 86, 172, 223, 57, 114, 228, 79, 158, 187, 241, 101, 202, 19, 38, 76,
    152, 183, 233, 85, 170, 211, 33, 66, 132, 143, 153, 181, 237, 93, 186, 243,
    97, 194, 3, 6, 12, 24, 48, 96, 192, 7, 14, 28, 56, 112, 224, 71,
    142, 155, 177, 229, 77, 154, 179, 225, 69, 138, 147, 161, 197, 13, 26, 52,
    104, 208, 39, 78, 156, 191, 249, 117, 234, 83, 166, 203, 17, 34, 68, 136,
    151, 169, 213, 45, 90, 180, 239, 89, 178, 227, 65, 130, 131, 129, 133, 141,
    157, 189, 253, 125, 250, 115, 230, 75, 150, 171, 209, 37, 74, 148, 175, 217,
    53, 106, 212, 47, 94, 188, 255, 121, 242, 99, 198, 11, 22, 44, 88, 176,
    231, 73, 146, 163, 193, 5, 10, 20, 40, 80, 160, 199, 9, 18, 36, 72,
    144, 167, 201, 21, 42, 84, 168, 215, 41, 82, 164, 207, 25, 50, 100, 200,
    23, 46, 92, 184, 247, 105, 210, 35, 70, 140, 159, 185, 245, 109, 218, 51,
    102, 204, 31, 62, 124, 248, 119, 238, 91, 182, 235, 81, 162, 195
};
static const uint8_t gf_log[256] = {
    0, 0, 1, 99, 2, 198, 100, 106, 3, 205, 199, 188, 101, 126, 107, 42,
    4, 141, 206, 78, 200, 212, 189, 225, 102, 221, 127, 49, 108, 32, 43, 243,
    5, 87, 142, 232, 207, 172, 79, 131, 201, 217, 213, 65, 190, 148, 226, 180,
    103, 39, 222, 240, 128, 177, 50, 53, 109, 69, 33, 18, 44, 13, 244, 56,
    6, 155, 88, 26, 143, 121, 233, 112, 208, 194, 173, 168, 80, 117, 132, 72,
    202, 252, 218, 138, 214, 84, 66, 36, 191, 152, 149, 249, 227, 94, 181, 21,
    104, 97, 40, 186, 223, 76, 241, 47, 129, 230, 178, 63, 51, 238, 54, 16,
    110, 24, 70, 166, 34, 136, 19, 247, 45, 184, 14, 61, 245, 164, 57, 59,
    7, 158, 156, 157, 89, 159, 27, 8, 144, 9, 122, 28, 234, 160, 113, 90,
    209, 29, 195, 123, 174, 10, 169, 145, 81, 91, 118, 114, 133, 161, 73, 235,
    203, 124, 253, 196, 219, 30, 139, 210, 215, 146, 85, 170, 67, 11, 37, 175,
    192, 115, 153, 119, 150, 92, 250, 82, 228, 236, 95, 74, 182, 162, 22, 134,
    105, 197, 98, 254, 41, 125, 187, 204, 224, 211, 77, 140, 242, 31, 48, 220,
    130, 171, 231, 86, 179, 147, 64, 216, 52, 176, 239, 38, 55, 12, 17, 68,
    111, 120, 25, 154, 71, 116, 167, 193, 35, 83, 137, 251, 20, 93, 248, 151,
    46, 75, 185, 96, 15, 237, 62, 229, 246, 135, 165, 23, 58, 163, 60, 183
};
static uint8_t mul(uint8_t a, uint8_t b)
{
    return a && b ? gf_exp[gf_log[a] + gf_log[b]] : 0;
}
static uint8_t power(uint8_t a, int n)
{
    return !n ? 1 : !a ? 0 : gf_exp[(gf_log[a] * n) % 255];
}
static uint8_t divgf(uint8_t a, uint8_t b)
{
    return a && b ? gf_exp[gf_log[a] + 255 - gf_log[b]] : 0;
}
static int roots(int n)
{
    return n == 30 ? 12 : n == 48 ? 14 : n == 92 ? 20 : 0;
}
static bool synd(const uint8_t *f, int n, uint8_t *s, int r)
{
    bool bad = false;
    for (int j = 0; j < r; j++) {
        uint8_t x = power(2, 120 + j), v = 0;
        for (int i = 0; i < n; i++)
            v = mul(v, x) ^ f[i];
        s[j] = v;
        bad |= v != 0;
    }
    return bad;
}
bool uat_rs_encode(const uint8_t *data, int k, uint8_t *f)
{
    int r = k == 18 ? 12 : k == 34 ? 14 : k == 72 ? 20 : 0;
    if (!data || !f || !r) return false;
    uint8_t g[21] = {1}, work[92] = {0};
    int degree = 0;
    for (int j = 0; j < r; j++) {
        uint8_t x = power(2, 120 + j);
        g[degree + 1] = 0;
        for (int i = degree + 1; i > 0; i--)
            g[i] = g[i] ^ mul(g[i - 1], x);
        degree++;
    }
    memcpy(work, data, k);
    for (int i = 0; i < k; i++) {
        uint8_t v = work[i];
        for (int j = 1; j <= r; j++)
            work[i + j] ^= mul(v, g[j]);
    }
    memmove(f, data, k);
    memcpy(f + k, work + k, r);
    return true;
}
int uat_rs_decode(uint8_t *f, int n)
{
    int r = roots(n);
    if (!f || !r) return -1;
    uint8_t s[20], c[21] = {1}, b[21] = {1}, old[21];
    if (!synd(f, n, s, r)) return 0;
    int l = 0, m = 1;
    uint8_t scale = 1;
    for (int step = 0; step < r; step++) {
        uint8_t d = s[step];
        for (int i = 1; i <= l; i++)
            d ^= mul(c[i], s[step - i]);
        if (!d) {
            m++;
            continue;
        }
        memcpy(old, c, sizeof(c));
        uint8_t q = divgf(d, scale);
        for (int i = 0; i + m <= r; i++)
            c[i + m] ^= mul(q, b[i]);
        if (2 * l <= step) {
            l = step + 1 - l;
            memcpy(b, old, sizeof(b));
            scale = d;
            m = 1;
        } else
            m++;
    }
    if (!l || l > r / 2) return -1;
    int pos[10], found = 0;
    uint8_t xs[10];
    for (int i = 0; i < n; i++) {
        uint8_t x = power(2, n - 1 - i), z = power(x, 254), v = c[l];
        for (int j = l - 1; j >= 0; j--)
            v = mul(v, z) ^ c[j];
        if (!v) {
            if (found >= l) return -1;
            pos[found] = i;
            xs[found++] = x;
        }
    }
    if (found != l) return -1;
    /* Solve the first l syndromes for error magnitudes, then verify every
       syndrome before accepting any mutation. */
    uint8_t a[10][11] = {{0}}, copy[92];
    memcpy(copy, f, n);
    for (int row = 0; row < l; row++) {
        for (int col = 0; col < l; col++)
            a[row][col] = power(xs[col], 120 + row);
        a[row][l] = s[row];
    }
    for (int col = 0; col < l; col++) {
        int p = col;
        while (p < l && !a[p][col])
            p++;
        if (p == l) return -1;
        for (int j = col; j <= l; j++) {
            uint8_t t = a[col][j];
            a[col][j] = a[p][j];
            a[p][j] = t;
        }
        uint8_t q = a[col][col];
        for (int j = col; j <= l; j++)
            a[col][j] = divgf(a[col][j], q);
        for (int i = 0; i < l; i++)
            if (i != col) {
                q = a[i][col];
                for (int j = col; j <= l; j++)
                    a[i][j] ^= mul(q, a[col][j]);
            }
    }
    for (int i = 0; i < l; i++)
        copy[pos[i]] ^= a[i][l];
    if (synd(copy, n, s, r)) return -1;
    memcpy(f, copy, n);
    return l;
}
bool uat_frame_decode(const uint8_t *f, int n, uat_fix_t *fix, int *corrected)
{
    if (!f || !fix || (n != 30 && n != 48)) return false;
    uint8_t copy[48];
    memcpy(copy, f, n);
    int e = uat_rs_decode(copy, n);
    if (n == 48 && (e < 0 || (copy[0] >> 3) == 0)) {
        memcpy(copy, f, 30);
        n = 30;
        e = uat_rs_decode(copy, n);
    }
    if (e < 0 || ((copy[0] >> 3) == 0) != (n == 30)) return false;
    memset(fix, 0, sizeof(*fix));
    fix->type = copy[0] >> 3;
    fix->qualifier = copy[0] & 7;
    fix->address = ((uint32_t)copy[1] << 16) | ((uint32_t)copy[2] << 8) | copy[3];
    if (!fix->address || fix->address == 0xFFFFFF) return false;
    uint32_t lat = ((uint32_t)copy[4] << 15) | ((uint32_t)copy[5] << 7) | (copy[6] >> 1);
    uint32_t lon = ((uint32_t)(copy[6] & 1) << 23) | ((uint32_t)copy[7] << 15) | ((uint32_t)copy[8] << 7) |
                   (copy[9] >> 1);
    fix->position = fix->type <= 10 && ((copy[11] & 15) || lat || lon);
    fix->lat = lat * (360.0 / 16777216.0);
    if (fix->lat > 90) fix->lat -= 180;
    fix->lon = lon * (360.0 / 16777216.0);
    if (fix->lon > 180) fix->lon -= 360;
    if (fix->type <= 10) {
        unsigned alt = ((unsigned)copy[10] << 4) | (copy[11] >> 4);
        fix->altitude_valid = alt != 0;
        fix->altitude = alt ? (int)(alt - 1) * 25 - 1000 : 0;
        fix->airground = copy[12] >> 6;
        if (fix->airground < 2) {
            unsigned ns = ((unsigned)(copy[12] & 31) << 6) | (copy[13] >> 2);
            unsigned ew = ((unsigned)(copy[13] & 3) << 9) | ((unsigned)copy[14] << 1) | (copy[15] >> 7);
            fix->velocity_valid = (ns & 1023) && (ew & 1023);
            int factor = fix->airground == 1 ? 4 : 1;
            fix->ns_velocity = ((int)(ns & 1023) - 1) * factor * ((ns & 1024) ? -1 : 1);
            fix->ew_velocity = ((int)(ew & 1023) - 1) * factor * ((ew & 1024) ? -1 : 1);
            unsigned vr = ((unsigned)(copy[15] & 127) << 4) | (copy[16] >> 4);
            fix->vert_rate_valid = (vr & 511) != 0;
            fix->vert_rate = ((int)(vr & 511) - 1) * 64 * ((vr & 512) ? -1 : 1);
        }
    }
    if (fix->type == 1 || fix->type == 3) {
        static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ ";
        unsigned digits[8];
        unsigned first = ((unsigned)copy[17] << 8) | copy[18];
        fix->emitter_category = first / 1600;
        digits[0] = first / 40 % 40;
        digits[1] = first % 40;
        for (int word = 0; word < 2; word++) {
            unsigned v = ((unsigned)copy[19 + word * 2] << 8) | copy[20 + word * 2];
            digits[2 + word * 3] = v / 1600;
            digits[3 + word * 3] = v / 40 % 40;
            digits[4 + word * 3] = v % 40;
        }
        /* CSID, byte 27 bit 7, distinguishes a callsign from a flight-plan
           identifier. Do not publish the latter as an aircraft callsign. */
        bool valid = (copy[26] & 2) && fix->emitter_category < 40 && digits[0] < 36;
        for (int i = 0; i < 8; i++) {
            if (digits[i] > 36) valid = false;
            fix->callsign[i] = digits[i] <= 36 ? alphabet[digits[i]] : 0;
        }
        if (!valid) fix->callsign[0] = 0;
        else for (int i = 7; i >= 0 && fix->callsign[i] == ' '; i--) fix->callsign[i] = 0;
    }
    if (corrected) *corrected = e;
    return true;
}

int uat_uplink_decode(const uint8_t *wire, uint8_t *payload)
{
    if (!wire || !payload) return -1;
    uint8_t block[92], decoded[432];
    int corrected = 0;
    for (int lane = 0; lane < 6; lane++) {
        for (int j = 0; j < 92; j++) block[j] = wire[j * 6 + lane];
        int n = uat_rs_decode(block, 92);
        if (n < 0) return -1;
        corrected += n;
        memcpy(decoded + lane * 72, block, 72);
    }
    memcpy(payload, decoded, sizeof(decoded));
    return corrected;
}
