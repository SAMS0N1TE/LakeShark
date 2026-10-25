/* See ls_mdns.h. */

#include "ls_mdns.h"

#include <ctype.h>
#include <string.h>

enum { TYPE_A = 1, TYPE_ANY = 255, CLASS_IN = 1, CLASS_ANY = 255, TTL = 120, TTL_LEGACY = 10 };

static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

/* A name at off, dotted, lower case; *end is where the record goes on.
   Pointers are followed, a few at most, so a loop ends. */
static bool read_name(const uint8_t *pkt, size_t n, size_t off, char *out, size_t cap, size_t *end)
{
    size_t k = 0;
    int hops = 0;
    bool jumped = false;
    for (;;) {
        if (off >= n) return false;
        const uint8_t len = pkt[off];
        if ((len & 0xC0) == 0xC0) {
            if (off + 1 >= n || ++hops > 8) return false;
            if (!jumped) *end = off + 2;
            jumped = true;
            off = (size_t)(len & 0x3F) << 8 | pkt[off + 1];
            continue;
        }
        if (len & 0xC0) return false;
        if (!len) {
            if (!jumped) *end = off + 1;
            out[k] = 0;
            return true;
        }
        if (off + 1 + len > n || k + len + 2 > cap) return false;
        if (k) out[k++] = '.';
        for (size_t i = 0; i < len; i++) out[k++] = (char)tolower(pkt[off + 1 + i]);
        off += 1 + len;
    }
}

typedef struct {
    uint8_t *p;
    size_t n, cap;
    bool over;
} out_t;

static void put(out_t *o, const void *b, size_t k)
{
    if (o->n + k > o->cap) { o->over = true; return; }
    memcpy(o->p + o->n, b, k);
    o->n += k;
}

static void put16(out_t *o, unsigned v)
{
    const uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    put(o, b, 2);
}

static void put_name(out_t *o, const char *host)
{
    const size_t len = strlen(host) < 63 ? strlen(host) : 63;
    const uint8_t l = (uint8_t)len;
    put(o, &l, 1);
    put(o, host, len);
    put(o, "\x05" "local", 7);                       /* and the root's zero */
}

/* host.local's A record. */
static void put_answer(out_t *o, const char *host, uint32_t ip, bool legacy)
{
    put_name(o, host);
    put16(o, TYPE_A);
    put16(o, legacy ? CLASS_IN : 0x8000 | CLASS_IN);   /* cache-flush: this is the only address */
    put16(o, 0);
    put16(o, legacy ? TTL_LEGACY : TTL);
    put16(o, 4);
    put(o, &ip, 4);                                    /* already in network order */
}

size_t ls_mdns_answer(const uint8_t *pkt, size_t n, const char *host, uint32_t ip, bool legacy,
                      uint8_t *out, size_t cap, bool *unicast)
{
    *unicast = false;
    if (n < 12 || (pkt[2] & 0xF8)) return 0;         /* an answer, or not a standard query */
    char want[72], name[256];
    const size_t hl = strlen(host) < 63 ? strlen(host) : 63;
    for (size_t i = 0; i < hl; i++) want[i] = (char)tolower((unsigned char)host[i]);
    memcpy(want + hl, ".local", 7);

    const unsigned qd = get16(pkt + 4);
    size_t off = 12;
    bool asked = false;
    for (unsigned q = 0; q < qd && q < 32; q++) {
        size_t end = 0;
        if (!read_name(pkt, n, off, name, sizeof(name), &end) || end + 4 > n) return 0;
        const unsigned type = get16(pkt + end), klass = get16(pkt + end + 2);
        off = end + 4;
        if (strcmp(name, want) || (type != TYPE_A && type != TYPE_ANY) ||
            ((klass & 0x7FFF) != CLASS_IN && (klass & 0x7FFF) != CLASS_ANY))
            continue;
        asked = true;
        if (klass & 0x8000) *unicast = true;
    }
    if (!asked) return 0;

    out_t o = {.p = out, .cap = cap};
    put16(&o, legacy ? get16(pkt) : 0);              /* the id: a legacy asker matches on it */
    put16(&o, 0x8400);                               /* an answer, authoritative */
    put16(&o, legacy ? 1 : 0);
    put16(&o, 1);
    put16(&o, 0);
    put16(&o, 0);
    if (legacy) {
        put_name(&o, host);
        put16(&o, TYPE_A);
        put16(&o, CLASS_IN);
    }
    put_answer(&o, host, ip, legacy);
    return o.over ? 0 : o.n;
}

size_t ls_mdns_announce(const char *host, uint32_t ip, uint8_t *out, size_t cap)
{
    out_t o = {.p = out, .cap = cap};
    static const uint8_t head[12] = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    put(&o, head, sizeof(head));
    put_answer(&o, host, ip, false);
    return o.over ? 0 : o.n;
}
