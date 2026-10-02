/* Pulse timing at half-microsecond resolution. These are candidates, not
   authenticated interrogations or altitude/identity decodes. */
#include "aviation_decode.h"
static int bit(const uint8_t *b, int i)
{
    return (b[i / 8] >> (7 - i % 8)) & 1;
}
static int pulse(const uint8_t *b, int n, int at, int lo, int hi)
{
    for (int i = at - 1; i <= at + 1; i++) {
        if (i < 0 || i >= n || !bit(b, i) || (i && bit(b, i - 1))) continue;
        int j = i;
        while (j < n && bit(b, j))
            j++;
        if (j < n && j - i >= lo && j - i <= hi) return 1;
    }
    return 0;
}
av_class_t aviation_classify(const uint8_t *b, size_t bytes, int band)
{
    if (!b || bytes < 12) return AV_UNKNOWN;
    int n = (int)bytes * 8;
    if (band == 1030) {
        if (!pulse(b, n, 0, 1, 2)) return AV_UNKNOWN;
        /* P2 at 2 us is optional for A/C. P6 begins at 3.5 us. */
        if (pulse(b, n, 4, 1, 2) && pulse(b, n, 7, 30, 65)) return AV_S;
        if (pulse(b, n, 16, 1, 2)) return AV_A;
        if (pulse(b, n, 42, 1, 2)) return AV_C;
    } else if (band == 1090) {
        if (pulse(b, n, 0, 1, 2) && pulse(b, n, 41, 1, 2)) return AV_AC;
    } else {
        if (!pulse(b, n, 0, 6, 8)) return AV_UNKNOWN;
        if (pulse(b, n, 24, 6, 8)) return AV_X;
        if (pulse(b, n, 72, 6, 8)) return AV_Y;
    }
    return AV_UNKNOWN;
}
int aviation_df(const uint8_t *b, size_t bytes)
{
    if (!b || bytes < 14) return -1;
    int df = 0;
    for (int i = 0; i < 5; i++) {
        int a = bit(b, 2 * i), c = bit(b, 2 * i + 1);
        if (a == c) return -1;
        df = (df << 1) | a;
    }
    int bits = (df & 16) ? 112 : 56;
    if (bytes * 8 < (size_t)bits * 2) return -1;
    for (int i = 5; i < bits; i++)
        if (bit(b, 2 * i) == bit(b, 2 * i + 1)) return -1;
    return df;
}
