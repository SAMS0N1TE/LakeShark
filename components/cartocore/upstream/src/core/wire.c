#include "cartocore/mvt.h"
cc_wire cc_wire_init(const void *data, size_t size) {
    static const uint8_t empty = 0;
    cc_wire r;
    r.p = data ? data : &empty; r.end = r.p + (data ? size : 0);
    r.error = !data && size != 0; return r;
}
int cc_varint(cc_wire *r, uint64_t *out) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 10; ++i) {
        uint8_t b;
        if (r->p == r->end) break;
        b = *r->p++;
        if (i == 9 && b > 1) break;
        v |= (uint64_t)(b & 127) << (7 * i);
        if (!(b & 128)) { *out = v; return 1; }
    }
    r->error = 1; return 0;
}
int64_t cc_zigzag(uint64_t v) {
    return (v & 1) ? -(int64_t)(v >> 1) - 1 : (int64_t)(v >> 1);
}
int cc_wire_next(cc_wire *r, cc_field *f) {
    uint64_t tag, n;
    f->bytes.data = 0; f->bytes.size = 0; f->integer = 0;
    if (r->error || r->p == r->end) return 0;
    if (!cc_varint(r, &tag) || (tag >> 3) == 0 || (tag >> 3) > 0x1fffffff) goto bad;
    f->number = (uint32_t)(tag >> 3); f->type = (uint32_t)(tag & 7);
    switch (f->type) {
    case 0: return cc_varint(r, &f->integer);
    case 1: n = 8; break;
    case 5: n = 4; break;
    case 2: if (!cc_varint(r, &n)) return 0; break;
    default: goto bad;
    }
    if (n > (uint64_t)(r->end - r->p)) goto bad;
    f->bytes.data = r->p; f->bytes.size = (size_t)n;
    r->p += (size_t)n; return 1;
bad: r->error = 1; return 0;
}
