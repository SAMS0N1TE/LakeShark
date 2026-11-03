/* See ls_ws.h. */

#include "ls_ws.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

/* --- the handshake --------------------------------------------------------- */

/* SHA-1, for the one thing a WebSocket wants it for. */
static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        const uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
        const uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const uint8_t *p, size_t n, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint64_t bits = (uint64_t)n * 8;
    for (; n >= 64; n -= 64, p += 64) sha1_block(h, p);
    uint8_t last[128] = {0};
    memcpy(last, p, n);
    last[n] = 0x80;
    const size_t len = n < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++) last[len - 1 - i] = (uint8_t)(bits >> (i * 8));
    sha1_block(h, last);
    if (len == 128) sha1_block(h, last + 64);
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - (i % 4) * 8));
}

void ls_ws_accept(const char *key, char out[29])
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t in[128], d[21] = {0};
    size_t k = strlen(key);
    if (k > sizeof(in) - sizeof(guid)) k = sizeof(in) - sizeof(guid);
    memcpy(in, key, k);
    memcpy(in + k, guid, sizeof(guid) - 1);
    sha1(in, k + sizeof(guid) - 1, d);
    char *o = out;
    for (int i = 0; i < 21; i += 3) {
        const uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2];
        *o++ = b64[v >> 18 & 63];
        *o++ = b64[v >> 12 & 63];
        *o++ = i + 1 < 20 ? b64[v >> 6 & 63] : '=';
        *o++ = i + 2 < 20 ? b64[v & 63] : '=';
    }
    out[28] = 0;
}

/* --- frames ---------------------------------------------------------------- */

size_t ls_ws_header(uint8_t out[10], int opcode, size_t n)
{
    out[0] = (uint8_t)(0x80 | (opcode & 15));
    if (n < 126) { out[1] = (uint8_t)n; return 2; }
    if (n < 65536) { out[1] = 126; out[2] = (uint8_t)(n >> 8); out[3] = (uint8_t)n; return 4; }
    out[1] = 127;
    for (int i = 0; i < 8; i++) out[2 + i] = (uint8_t)((uint64_t)n >> (56 - i * 8));
    return 10;
}

int ls_ws_parse(uint8_t *buf, size_t n, size_t max, ls_ws_msg *m)
{
    if (n < 2) return 0;
    /* No extensions/fragment reassembly negotiated by this endpoint. */
    int op = buf[0] & 15;
    if ((buf[0] & 0x70) || !(buf[0] & 0x80) ||
        (op != LS_WS_TEXT && op != LS_WS_BINARY && op != LS_WS_CLOSE &&
         op != LS_WS_PING && op != LS_WS_PONG)) return -1;
    if (!(buf[1] & 0x80)) return -1;                /* a browser masks everything it sends */
    size_t at = 2;
    uint64_t len = buf[1] & 0x7F;
    if (len == 126) {
        if (n < 4) return 0;
        len = (uint64_t)buf[2] << 8 | buf[3];
        if (len < 126) return -1;
        at = 4;
    } else if (len == 127) {
        if (n < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | buf[2 + i];
        if (len < 65536 || (buf[2] & 0x80)) return -1;
        at = 10;
    }
    if (len > max || len > INT_MAX - at - 4 ||
        (op >= LS_WS_CLOSE && (len > 125 || (op == LS_WS_CLOSE && len == 1)))) return -1;
    if (n < at + 4 + len) return 0;
    const uint8_t *mask = buf + at;
    uint8_t *p = buf + at + 4;
    for (size_t i = 0; i < len; i++) p[i] ^= mask[i & 3];
    m->op = buf[0] & 15;
    m->fin = buf[0] & 0x80;
    m->data = p;
    m->n = (size_t)len;
    return (int)(at + 4 + len);
}

/* --- requests -------------------------------------------------------------- */

bool ls_http_request(const char *req, char *method, size_t mcap, char *path, size_t pcap)
{
    const char *sp = strchr(req, ' ');
    if (!sp || (size_t)(sp - req) >= mcap || sp == req) return false;
    memcpy(method, req, (size_t)(sp - req));
    method[sp - req] = 0;
    const char *p = sp + 1;
    size_t k = strcspn(p, " ?\r\n");
    if (p[0] != '/' || k >= pcap) return false;
    memcpy(path, p, k);
    path[k] = 0;
    const char *proto = p + strcspn(p, " \r\n");
    return proto[0] == ' ' && !strncmp(proto + 1, "HTTP/1.", 7);
}

bool ls_http_header(const char *req, const char *name, char *out, size_t cap)
{
    const size_t nl = strlen(name);
    for (const char *l = strstr(req, "\r\n"); l && l[2] && !(l[2] == '\r' && l[3] == '\n'); l = strstr(l + 2, "\r\n")) {
        const char *h = l + 2;
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)name[i])) i++;
        if (i < nl || h[i] != ':') continue;
        const char *v = h + i + 1;
        while (*v == ' ' || *v == '\t') v++;
        size_t k = strcspn(v, "\r\n");
        while (k && (v[k - 1] == ' ' || v[k - 1] == '\t')) k--;
        if (k >= cap) return false;
        memcpy(out, v, k);
        out[k] = 0;
        return true;
    }
    return false;
}

bool ls_http_has_token(const char *value, const char *token)
{
    const size_t tl = strlen(token);
    for (const char *p = value; *p;) {
        while (*p == ' ' || *p == ',') p++;
        const size_t k = strcspn(p, ",");
        size_t e = k;
        while (e && p[e - 1] == ' ') e--;
        if (e == tl) {
            size_t i = 0;
            while (i < tl && tolower((unsigned char)p[i]) == tolower((unsigned char)token[i])) i++;
            if (i == tl) return true;
        }
        p += k;
    }
    return false;
}
