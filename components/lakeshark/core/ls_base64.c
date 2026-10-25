#include "ls_base64.h"
#include <string.h>
static int value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}
bool ls_base64_decode(const char *in, uint8_t *out, size_t cap, size_t *written) {
    if (!in || !out || !written)
        return false;
    size_t n = strlen(in), count = 0;
    if (n % 4)
        return false;
    for (size_t i = 0; i < n; i += 4) {
        int a = value(in[i]), b = value(in[i + 1]), c = value(in[i + 2]), d = value(in[i + 3]);
        bool p2 = in[i + 2] == '=', p3 = in[i + 3] == '=';
        if (a < 0 || b < 0 || (!p2 && c < 0) || (!p3 && d < 0) || (p2 && !p3) ||
            ((p2 || p3) && i + 4 != n) || (p2 && (b & 15)) || (!p2 && p3 && (c & 3)))
            return false;
        size_t add = p2 ? 1 : p3 ? 2 : 3;
        if (add > cap - count)
            return false;
        uint32_t bits = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)(p2 ? 0 : c) << 6 |
                        (uint32_t)(p3 ? 0 : d);
        out[count++] = (uint8_t)(bits >> 16);
        if (add > 1)
            out[count++] = (uint8_t)(bits >> 8);
        if (add > 2)
            out[count++] = (uint8_t)bits;
    }
    *written = count;
    return true;
}
