/* Independent UAT RS implementation. Parameters checked against
   https://github.com/mutability/dump978/blob/master/fec.c (GPL-2.0-or-later).
   dump978 itself is GPL; its bundled Phil Karn libfec is LGPL (fec/README.fec).
   No upstream implementation is copied here. */
#include "uat_decode.h"
#include <string.h>
static uint8_t mul(uint8_t a, uint8_t b)
{
    unsigned v = 0, x = a;
    while (b) {
        if (b & 1) v ^= x;
        b >>= 1;
        x <<= 1;
        if (x & 256) x ^= 0x187;
    }
    return (uint8_t)v;
}
static uint8_t power(uint8_t a, int n)
{
    uint8_t v = 1;
    while (n) {
        if (n & 1) v = mul(v, a);
        a = mul(a, a);
        n >>= 1;
    }
    return v;
}
static uint8_t divgf(uint8_t a, uint8_t b)
{
    return mul(a, power(b, 254));
}
static int roots(int n)
{
    return n == 30 ? 12 : n == 48 ? 14 : 0;
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
    int r = k == 18 ? 12 : k == 34 ? 14 : 0;
    if (!data || !f || !r) return false;
    uint8_t g[15] = {1}, work[48] = {0};
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
    uint8_t s[14], c[15] = {1}, b[15] = {1}, old[15];
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
    int pos[7], found = 0;
    uint8_t xs[7];
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
    uint8_t a[7][8] = {{0}}, copy[48];
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
    if (!f || !fix || !roots(n)) return false;
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
    uint32_t lat = ((uint32_t)copy[4] << 15) | ((uint32_t)copy[5] << 7) | (copy[6] >> 1);
    uint32_t lon = ((uint32_t)(copy[6] & 1) << 23) | ((uint32_t)copy[7] << 15) | ((uint32_t)copy[8] << 7) |
                   (copy[9] >> 1);
    fix->position = fix->type <= 10 && ((copy[11] & 15) || lat || lon);
    fix->lat = lat * (360.0 / 16777216.0);
    if (fix->lat > 90) fix->lat -= 180;
    fix->lon = lon * (360.0 / 16777216.0);
    if (fix->lon > 180) fix->lon -= 360;
    if (corrected) *corrected = e;
    return true;
}
