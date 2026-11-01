#include "ble_hid_report.h"
#include <limits.h>
#include <string.h>

typedef struct {
    uint32_t page, size, count, id;
    int32_t min, max;
} globals_t;
static int32_t signed_item(uint32_t v, unsigned n)
{
    if (!n)
        return 0;
    if (n < 4 && (v & (1u << (n * 8 - 1))))
        v |= UINT32_MAX << (n * 8);
    return (int32_t)v;
}
static bool supported(uint16_t p, uint16_t u, uint8_t flags)
{
    return p == 7 || p == 9 || p == 12 ||
           (p == 1 && (flags & 4) && (u == 0x30 || u == 0x31 || u == 0x38));
}
hid_parse_result_t hid_map_compile(hid_map_t *m, const uint8_t *b, size_t len)
{
    if (!m || !b || !len)
        return HID_PARSE_MALFORMED;
    memset(m, 0, sizeof(*m));
    if (len > HID_MAP_MAX)
        return HID_PARSE_LIMIT;
    globals_t g = {0}, stack[4];
    unsigned sp = 0, depth = 0;
    uint32_t usages[64], lo = 0, hi = 0;
    unsigned nu = 0;
    bool range = false, have_lo = false;
    m->fingerprint = 2166136261u;
    for (size_t i = 0; i < len; i++)
        m->fingerprint = (m->fingerprint ^ b[i]) * 16777619u;
    for (size_t i = 0; i < len;) {
        uint8_t h = b[i++];
        if (h == 0xfe)
            return HID_PARSE_UNSUPPORTED; /* long items have no supported semantics */
        unsigned n = h & 3;
        if (n == 3)
            n = 4;
        if (n > len - i)
            return HID_PARSE_MALFORMED;
        uint32_t v = 0;
        for (unsigned j = 0; j < n; j++)
            v |= (uint32_t)b[i++] << (8 * j);
        unsigned type = (h >> 2) & 3, tag = h >> 4;
        if (type == 1) {
            switch (tag) {
            case 0:
                if (v > 65535)
                    return HID_PARSE_LIMIT;
                g.page = v;
                break;
            case 1:
                g.min = signed_item(v, n);
                break;
            case 2:
                g.max = g.min < 0 ? signed_item(v, n) : (int32_t)v;
                break;
            case 7:
                g.size = v;
                break;
            case 8:
                if (!v || v > 255)
                    return HID_PARSE_MALFORMED;
                g.id = v;
                break;
            case 9:
                g.count = v;
                break;
            case 10:
                if (sp == 4)
                    return HID_PARSE_LIMIT;
                stack[sp++] = g;
                break;
            case 11:
                if (!sp)
                    return HID_PARSE_MALFORMED;
                g = stack[--sp];
                break;
            default:
                break;
            }
        } else if (type == 2) {
            uint32_t u = n == 4 ? v : (g.page << 16) | v;
            if (tag == 0) {
                if (nu == 64)
                    return HID_PARSE_LIMIT;
                usages[nu++] = u;
            } else if (tag == 1) {
                lo = u;
                have_lo = true;
            } else if (tag == 2) {
                if (!have_lo || u < lo || (u >> 16) != (lo >> 16))
                    return HID_PARSE_MALFORMED;
                hi = u;
                range = true;
            } else if (tag == 10 && v)
                return HID_PARSE_UNSUPPORTED; /* delimiters */
        } else if (type == 0) {
            if (tag == 10) {
                if (++depth > 16)
                    return HID_PARSE_LIMIT;
            } else if (tag == 12) {
                if (!depth)
                    return HID_PARSE_MALFORMED;
                depth--;
            } else if (tag == 8 || tag == 9 || tag == 11) {
                uint8_t rt = tag == 8 ? 1 : tag == 9 ? 2 : 3;
                if (!g.size || g.size > 32 || !g.count || g.count > 512)
                    return HID_PARSE_LIMIT;
                uint32_t bits = g.size * g.count;
                unsigned r = 0;
                while (r < m->nreports && (m->reports[r].id != g.id || m->reports[r].type != rt))
                    r++;
                if (r == m->nreports) {
                    if (r == HID_REPORTS_MAX)
                        return HID_PARSE_LIMIT;
                    m->nreports++;
                    m->reports[r] = (hid_report_size_t){g.id, rt, 0};
                }
                unsigned start = m->reports[r].bits;
                if (bits > HID_PAYLOAD_MAX * 8 || start + bits > HID_PAYLOAD_MAX * 8)
                    return HID_PARSE_LIMIT;
                m->reports[r].bits += bits;
                if (!(v & 1)) {
                    /* Arrays retain their range. Variables with explicit usages compile
                     * separately; uniform ranges stay compressed (NKRO is one field). */
                    unsigned nf = (v & 2) && nu ? g.count : 1;
                    if (nf > (unsigned)(HID_FIELDS_MAX - m->nfields))
                        return HID_PARSE_LIMIT;
                    for (unsigned k = 0; k < nf; k++) {
                        uint32_t u = nu ? usages[k < nu ? k : nu - 1] : range ? lo : (g.page << 16);
                        uint32_t end = range ? hi : u;
                        if (nf > 1)
                            end = u;
                        hid_field_t *f = &m->fields[m->nfields++];
                        *f = (hid_field_t){.page = u >> 16,
                                           .usage = u & 65535,
                                           .usage_max = end & 65535,
                                           .bit = start + (nf > 1 ? k * g.size : 0),
                                           .count = nf > 1 ? 1 : g.count,
                                           .minimum = g.min,
                                           .maximum = g.max,
                                           .size = g.size,
                                           .id = g.id,
                                           .type = rt,
                                           .flags = v};
                        if (!supported(f->page, f->usage, f->flags))
                            m->unsupported++;
                    }
                }
            }
            nu = 0;
            range = have_lo = false;
            lo = hi = 0;
        }
    }
    if (depth || sp || !m->nreports)
        return HID_PARSE_MALFORMED;
    return HID_PARSE_OK;
}
static int32_t field_value(const uint8_t *b, unsigned bit, unsigned size, bool sign)
{
    uint32_t v = 0;
    for (unsigned j = 0; j < size; j++)
        v |= (uint32_t)((b[(bit + j) / 8] >> ((bit + j) % 8)) & 1) << j;
    if (sign && size < 32 && (v & (1u << (size - 1))))
        v |= UINT32_MAX << size;
    return (int32_t)v;
}
static int32_t add_axis(int32_t a, int32_t b)
{
    int64_t sum = (int64_t)a + b;
    return sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : (int32_t)sum;
}
bool hid_map_decode(const hid_map_t *m, uint8_t id, const uint8_t *b, size_t len, hid_sample_t *o)
{
    if (!m || !b || !o || len > HID_PAYLOAD_MAX)
        return false;
    memset(o, 0, sizeof(*o));
    bool found = false;
    for (unsigned r = 0; r < m->nreports; r++)
        if (m->reports[r].id == id && m->reports[r].type == 1) {
            if (len * 8 < m->reports[r].bits)
                return false;
            found = true;
        }
    if (!found)
        return false;
    for (unsigned k = 0; k < m->nfields; k++) {
        const hid_field_t *f = &m->fields[k];
        if (f->id != id || f->type != 1)
            continue;
        for (unsigned j = 0; j < f->count; j++) {
            int32_t v = field_value(b, f->bit + j * f->size, f->size, f->minimum < 0);
            unsigned u = f->usage + j;
            if (u > f->usage_max)
                u = f->usage_max;
            if (!(f->flags & 2)) {
                if (v < f->minimum || v > f->maximum)
                    continue;
                u = f->usage + (uint32_t)((int64_t)v - f->minimum);
                if (u > f->usage_max)
                    continue;
            } else if (!v && (f->page == 7 || f->page == 9 || f->page == 12))
                continue;
            if (f->page == 7) {
                o->keyboard = true;
                if (u >= 1 && u <= 3) {
                    o->rollover = true;
                    continue;
                }
                if (u < 256 && u)
                    o->keys[u / 8] |= 1u << (u % 8);
            } else if (f->page == 9) {
                o->pointer = true;
                if (u >= 1 && u <= 8 && v)
                    o->buttons |= 1u << (u - 1);
            } else if (f->page == 1 && (f->flags & 4)) {
                o->pointer = true;
                if (u == 0x30)
                    o->x = add_axis(o->x, v);
                else if (u == 0x31)
                    o->y = add_axis(o->y, v);
                else if (u == 0x38)
                    o->wheel = add_axis(o->wheel, v);
            } else if (f->page == 12) {
                if (u == 0x238 && (f->flags & 4)) {
                    o->pointer = true;
                    o->pan = add_axis(o->pan, v);
                } else if (v && o->nconsumer < 8)
                    o->consumer[o->nconsumer++] = u;
            }
        }
    }
    return !o->rollover;
}
bool hid_boot_decode(bool mouse, const uint8_t *b, size_t n, hid_sample_t *o)
{
    if (!b || !o || n < (mouse ? 3 : 8))
        return false;
    memset(o, 0, sizeof(*o));
    if (mouse) {
        o->pointer = true;
        o->buttons = b[0];
        o->x = (int8_t)b[1];
        o->y = (int8_t)b[2];
        if (n > 3)
            o->wheel = (int8_t)b[3];
    } else {
        o->keyboard = true;
        o->keys[28] = b[0];
        for (unsigned i = 2; i < 8; i++) {
            if (b[i] >= 1 && b[i] <= 3)
                return false;
            o->keys[b[i] / 8] |= 1u << (b[i] % 8);
        }
        o->keys[0] &= ~1u;
    }
    return true;
}
