/* BLE EXPLORER engine: the GATT walk, SIG names and decoders, JSON. See bx.h.
   The walk is: services, the characteristics of each, the descriptors of
   each characteristic that has room for them, then a read of every
   readable characteristic and every descriptor. One procedure at a time,
   as the board's client allows. */
#include "bx.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- the walk ------------------------------------------------------------------------- */

void bx_begin(bx_conn_t *c, int handle, const uint8_t addr[6], uint8_t addr_type, const char *name)
{
    memset(c, 0, sizeof(*c));
    c->handle = handle;
    c->state = BX_CONNECTING;
    memcpy(c->addr, addr, 6);
    c->addr_type = addr_type;
    snprintf(c->name, sizeof(c->name), "%s", name ? name : "");
}

void bx_rewalk(bx_conn_t *c)
{
    c->nsvc = c->nchr = c->ndsc = c->cur = 0;
    c->errors = 0;
    c->truncated = false;
    c->phase = BX_P_SERVICES;
}

static void uuid_in(bx_uuid_t *o, const lap_uuid_t *u)
{
    memset(o, 0, sizeof(*o));
    o->len = u->length == 16 || u->length == 4 ? u->length : 2;
    memcpy(o->b, u->bytes, o->len);
}

/* Characteristic ends: the handle before the next declaration in the same
   service, else the service's end. Sorted by value handle first. */
static void finish_chars(bx_conn_t *c)
{
    for (int i = 1; i < c->nchr; i++)
        for (int j = i; j > 0 && c->chr[j].value < c->chr[j - 1].value; j--) {
            const bx_chr_t t = c->chr[j];
            c->chr[j] = c->chr[j - 1];
            c->chr[j - 1] = t;
        }
    for (int i = 0; i < c->nchr; i++) {
        bx_chr_t *h = &c->chr[i];
        uint16_t end = h->svc < c->nsvc ? c->svc[h->svc].end : h->value;
        if (i + 1 < c->nchr && c->chr[i + 1].svc == h->svc && c->chr[i + 1].decl > h->value) end = c->chr[i + 1].decl - 1;
        h->end = end < h->value ? h->value : end;
    }
}

static void advance(bx_conn_t *c)
{
    switch (c->phase) {
    case BX_P_SERVICES: c->phase = BX_P_CHARS; c->cur = 0; break;
    default: c->cur++; break;
    }
}

bx_op_t bx_next(bx_conn_t *c)
{
    bx_op_t op = {0};
    if (c->state != BX_READY || c->busy) return op;
    for (;;) {
        switch (c->phase) {
        case BX_P_SERVICES:
            op.kind = BX_OP_DISCOVER;
            return op;
        case BX_P_CHARS:
            while (c->cur < c->nsvc && (!c->svc[c->cur].start || c->svc[c->cur].end < c->svc[c->cur].start)) c->cur++;
            if (c->cur < c->nsvc) { op.kind = BX_OP_DISCOVER; op.a = c->svc[c->cur].start; op.b = c->svc[c->cur].end; return op; }
            finish_chars(c);
            c->phase = BX_P_DESCS; c->cur = 0;
            break;
        case BX_P_DESCS:
            while (c->cur < c->nchr && c->chr[c->cur].end <= c->chr[c->cur].value) c->cur++;
            if (c->cur < c->nchr) { op.kind = BX_OP_DESCRIPTORS; op.a = c->chr[c->cur].value; op.b = c->chr[c->cur].end; return op; }
            c->phase = BX_P_READ_CHARS; c->cur = 0;
            break;
        case BX_P_READ_CHARS:
            while (c->cur < c->nchr && !(c->chr[c->cur].props & 0x02)) c->cur++;
            if (c->cur < c->nchr) { op.kind = BX_OP_READ; op.a = c->chr[c->cur].value; return op; }
            c->phase = BX_P_READ_DESCS; c->cur = 0;
            break;
        case BX_P_READ_DESCS:
            if (c->cur < c->ndsc) { op.kind = BX_OP_READ; op.a = c->dsc[c->cur].handle; return op; }
            c->phase = BX_P_DONE;
            break;
        default:
            return op;
        }
    }
}

void bx_started(bx_conn_t *c, const bx_op_t *op)
{
    c->busy = true;
    c->inflight = *op;
}

void bx_refused(bx_conn_t *c, const bx_op_t *op, int error)
{
    if (error == LAP_IO_EBUSY || error == LAP_IO_EAGAIN) return;      /* try again next frame */
    c->error = error;
    if (error == LAP_IO_ECLOSED) { c->state = BX_GONE; return; }
    c->errors++;
    if (!op->manual) advance(c);
}

static bx_chr_t *chr_by_value(bx_conn_t *c, uint16_t h)
{
    for (int i = 0; i < c->nchr; i++) if (c->chr[i].value == h) return &c->chr[i];
    return NULL;
}
static bx_dsc_t *dsc_by_handle(bx_conn_t *c, uint16_t h)
{
    for (int i = 0; i < c->ndsc; i++) if (c->dsc[i].handle == h) return &c->dsc[i];
    return NULL;
}

/* The procedure in flight ended (value, written or complete). */
static void ended(bx_conn_t *c, int32_t error)
{
    const bx_op_t op = c->inflight;
    c->busy = false;
    c->inflight.kind = BX_OP_NONE;
    if (error) {
        c->error = error;
        if (error == LAP_IO_ETIMEOUT) { c->state = BX_GONE; return; }     /* the board drops the link on a timeout */
        c->errors++;
    }
    if (!op.manual && op.kind != BX_OP_NONE) advance(c);
}

bool bx_event(bx_conn_t *c, const lap_ble_event_t *e, const uint8_t *data, size_t n)
{
    if (n > BX_VAL) n = BX_VAL;
    c->dropped = e->dropped;
    switch (e->kind) {
    case LAP_BLE_CONNECTED:
        c->busy = false;
        if (e->error) { c->state = BX_FAILED; c->error = e->error; }
        else { c->state = BX_READY; c->error = 0; bx_rewalk(c); }
        break;
    case LAP_BLE_DISCONNECTED:
        c->state = BX_GONE;
        c->busy = false;
        if (e->error) c->error = e->error;
        break;
    case LAP_GATT_SERVICE:
        if (c->nsvc >= BX_MAX_SVC) { c->truncated = true; break; }
        c->svc[c->nsvc].start = e->attribute;
        c->svc[c->nsvc].end = e->end_handle;
        uuid_in(&c->svc[c->nsvc].uuid, &e->uuid);
        c->nsvc++;
        break;
    case LAP_GATT_CHARACTERISTIC: {
        if (c->nchr >= BX_MAX_CHR) { c->truncated = true; break; }
        bx_chr_t *h = &c->chr[c->nchr];
        memset(h, 0, sizeof(*h));
        h->value = e->attribute;
        /* The board reports the declaration handle here; anything else (a
           simulator's end handle) falls back to the handle before the value. */
        h->decl = e->end_handle && e->end_handle < e->attribute ? e->end_handle : (uint16_t)(e->attribute - 1);
        h->props = (uint8_t)e->properties;
        uuid_in(&h->uuid, &e->uuid);
        h->svc = (uint8_t)(c->phase == BX_P_CHARS && c->cur < c->nsvc ? c->cur : 0);
        for (int i = 0; c->phase != BX_P_CHARS && i < c->nsvc; i++)
            if (h->value >= c->svc[i].start && h->value <= c->svc[i].end) h->svc = (uint8_t)i;
        c->nchr++;
        break;
    }
    case LAP_GATT_DESCRIPTOR: {
        if (chr_by_value(c, e->attribute)) break;               /* the value itself, if a stack reports it */
        if (c->ndsc >= BX_MAX_DSC) { c->truncated = true; break; }
        bx_dsc_t *d = &c->dsc[c->ndsc];
        memset(d, 0, sizeof(*d));
        d->handle = e->attribute;
        uuid_in(&d->uuid, &e->uuid);
        int owner = c->phase == BX_P_DESCS && c->cur < c->nchr ? c->cur : -1;
        for (int i = 0; owner < 0 && i < c->nchr; i++)
            if (d->handle > c->chr[i].value && d->handle <= c->chr[i].end) owner = i;
        d->chr = (uint8_t)(owner < 0 ? 0 : owner);
        if (owner >= 0 && bx_uuid16(&d->uuid) == 0x2902) c->chr[owner].cccd = d->handle;
        c->ndsc++;
        break;
    }
    case LAP_GATT_VALUE: {
        bx_chr_t *h = chr_by_value(c, e->attribute);
        bx_dsc_t *d = h ? NULL : dsc_by_handle(c, e->attribute);
        if (h) { h->error = e->error; if (!e->error) { memcpy(h->val, data, n); h->vlen = (uint8_t)n; h->has_value = true; } }
        if (d) { d->error = e->error; if (!e->error) { memcpy(d->val, data, n); d->vlen = (uint8_t)n; d->has_value = true; } }
        if (c->busy && c->inflight.kind == BX_OP_READ) ended(c, e->error);   /* a refused read is noted and the walk goes on */
        break;
    }
    case LAP_GATT_WRITTEN:
        if (c->busy && c->inflight.kind == BX_OP_SUBSCRIBE && !e->error) {
            for (int i = 0; i < c->nchr; i++)
                if (c->chr[i].cccd && c->chr[i].cccd == c->inflight.a) c->chr[i].notifying = c->inflight.on;
            bx_dsc_t *d = dsc_by_handle(c, c->inflight.a);
            if (d) { d->val[0] = c->inflight.on ? 1 : 0; d->val[1] = 0; d->vlen = 2; d->has_value = true; d->error = 0; }
        }
        if (c->busy) ended(c, e->error);
        break;
    case LAP_GATT_NOTIFY: {
        bx_chr_t *h = chr_by_value(c, e->attribute);
        if (h && !e->error) { memcpy(h->val, data, n); h->vlen = (uint8_t)n; h->has_value = true; h->notes++; }
        return !e->error;
    }
    case LAP_GATT_COMPLETE:
        if (c->busy) ended(c, e->error);
        break;
    default: break;
    }
    return false;
}

int bx_progress(const bx_conn_t *c)
{
    const int nc = c->nchr ? c->nchr : 1, nd = c->ndsc ? c->ndsc : 1, ns = c->nsvc ? c->nsvc : 1;
    switch (c->phase) {
    case BX_P_SERVICES: return 2;
    case BX_P_CHARS: return 5 + 25 * c->cur / ns;
    case BX_P_DESCS: return 30 + 30 * c->cur / nc;
    case BX_P_READ_CHARS: return 60 + 30 * c->cur / nc;
    case BX_P_READ_DESCS: return 90 + 10 * c->cur / nd;
    case BX_P_DONE: return 100;
    default: return 0;
    }
}

/* ---- names ----------------------------------------------------------------------------- */

static const uint8_t sig_base[12] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00};

uint16_t bx_uuid16(const bx_uuid_t *u)
{
    if (u->len == 2) return (uint16_t)(u->b[0] | u->b[1] << 8);
    if (u->len == 16 && !memcmp(u->b, sig_base, 12) && !u->b[14] && !u->b[15]) return (uint16_t)(u->b[12] | u->b[13] << 8);
    return 0;
}

void bx_uuid_text(const bx_uuid_t *u, char *out, size_t cap)
{
    const uint16_t s = bx_uuid16(u);
    if (s || u->len == 2) { snprintf(out, cap, "%04x", s); return; }
    if (u->len == 4) { snprintf(out, cap, "%02x%02x%02x%02x", u->b[3], u->b[2], u->b[1], u->b[0]); return; }
    const uint8_t *b = u->b;
    snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[15], b[14], b[13], b[12], b[11],
             b[10], b[9], b[8], b[7], b[6], b[5], b[4], b[3], b[2], b[1], b[0]);
}

typedef struct { uint16_t id; const char *name; } name_t;
static const name_t services[] = {
    {0x1800, "Generic Access"}, {0x1801, "Generic Attribute"}, {0x1802, "Immediate Alert"}, {0x1803, "Link Loss"},
    {0x1804, "Tx Power"}, {0x1805, "Current Time"}, {0x1809, "Health Thermometer"}, {0x180A, "Device Information"},
    {0x180D, "Heart Rate"}, {0x180F, "Battery"}, {0x1810, "Blood Pressure"}, {0x1812, "Human Interface Device"},
    {0x1814, "Running Speed and Cadence"}, {0x1816, "Cycling Speed and Cadence"}, {0x1818, "Cycling Power"},
    {0x1819, "Location and Navigation"}, {0x181A, "Environmental Sensing"}, {0x181C, "User Data"},
    {0x181D, "Weight Scale"}, {0x1826, "Fitness Machine"}, {0xFE59, "Nordic Secure DFU"},
};
static const name_t chars[] = {
    {0x2A00, "Device Name"}, {0x2A01, "Appearance"}, {0x2A04, "Preferred Connection Parameters"},
    {0x2A05, "Service Changed"}, {0x2A06, "Alert Level"}, {0x2A07, "Tx Power Level"}, {0x2A19, "Battery Level"},
    {0x2A1C, "Temperature Measurement"}, {0x2A1D, "Temperature Type"}, {0x2A23, "System ID"},
    {0x2A24, "Model Number"}, {0x2A25, "Serial Number"}, {0x2A26, "Firmware Revision"},
    {0x2A27, "Hardware Revision"}, {0x2A28, "Software Revision"}, {0x2A29, "Manufacturer Name"},
    {0x2A2A, "Regulatory Certification"}, {0x2A2B, "Current Time"}, {0x2A37, "Heart Rate Measurement"},
    {0x2A38, "Body Sensor Location"}, {0x2A39, "Heart Rate Control Point"}, {0x2A4A, "HID Information"},
    {0x2A4B, "Report Map"}, {0x2A4D, "Report"}, {0x2A4E, "Protocol Mode"}, {0x2A50, "PnP ID"},
    {0x2A53, "RSC Measurement"}, {0x2A5B, "CSC Measurement"}, {0x2A5C, "CSC Feature"},
    {0x2A63, "Cycling Power Measurement"}, {0x2A6D, "Pressure"}, {0x2A6E, "Temperature"}, {0x2A6F, "Humidity"},
    {0x2A9D, "Weight Measurement"}, {0x2AA6, "Central Address Resolution"}, {0x2B29, "Client Supported Features"},
    {0x2B2A, "Database Hash"},
};
static const name_t descs[] = {
    {0x2900, "Extended Properties"}, {0x2901, "User Description"}, {0x2902, "Client Char Config"},
    {0x2903, "Server Char Config"}, {0x2904, "Presentation Format"}, {0x2905, "Aggregate Format"},
    {0x2906, "Valid Range"}, {0x2908, "Report Reference"},
};
static const char *lookup(const name_t *t, size_t n, uint16_t id)
{
    for (size_t i = 0; i < n; i++) if (t[i].id == id) return t[i].name;
    return NULL;
}
/* Nordic UART Service, 6e40000X-b5a3-f393-e0a9-e50e24dcca9e: X 1 service, 2 RX, 3 TX. */
static int nus(const bx_uuid_t *u)
{
    static const uint8_t tail[12] = {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5};
    if (u->len != 16 || memcmp(u->b, tail, 12) || u->b[13] != 0x00 || u->b[14] != 0x40 || u->b[15] != 0x6e) return 0;
    return u->b[12];
}
const char *bx_service_name(const bx_uuid_t *u)
{
    if (nus(u) == 1) return "Nordic UART";
    const uint16_t s = bx_uuid16(u);
    return s ? lookup(services, sizeof(services) / sizeof(services[0]), s) : NULL;
}
const char *bx_char_name(const bx_uuid_t *u)
{
    if (nus(u) == 2) return "UART RX (write)";
    if (nus(u) == 3) return "UART TX (notify)";
    const uint16_t s = bx_uuid16(u);
    return s ? lookup(chars, sizeof(chars) / sizeof(chars[0]), s) : NULL;
}
const char *bx_desc_name(const bx_uuid_t *u)
{
    const uint16_t s = bx_uuid16(u);
    return s ? lookup(descs, sizeof(descs) / sizeof(descs[0]), s) : NULL;
}

void bx_props_text(uint8_t p, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s%s%s%s%s", p & 0x02 ? "R" : "", p & 0x08 ? "W" : "", p & 0x04 ? "w" : "", p & 0x10 ? "N" : "",
             p & 0x20 ? "I" : "", p & 0x01 ? "B" : "");
}

void bx_hex(const uint8_t *v, size_t n, char *out, size_t cap)
{
    size_t p = 0;
    out[0] = 0;
    for (size_t i = 0; i < n && p + 3 < cap; i++) p += (size_t)snprintf(out + p, cap - p, i ? " %02x" : "%02x", v[i]);
}

const char *bx_error_text(int32_t e)
{
    switch (e) {
    case 0: return "ok";
    case LAP_IO_EIO: return "refused (may need pairing)";
    case LAP_IO_ELIMIT: return "longer than 20 bytes";
    case LAP_IO_ETIMEOUT: return "timed out";
    case LAP_IO_EBUSY: return "busy";
    case LAP_IO_ECLOSED: return "closed";
    case LAP_IO_ENOTSUP: return "not supported here";
    case LAP_IO_EACCES: return "not allowed";
    case LAP_IO_EINVAL: return "bad request";
    default: return "error";
    }
}

/* ---- decoders ----------------------------------------------------------------------------- */

static unsigned u16(const uint8_t *v) { return (unsigned)(v[0] | v[1] << 8); }
static unsigned long u32(const uint8_t *v) { return (unsigned long)v[0] | (unsigned long)v[1] << 8 | (unsigned long)v[2] << 16 | (unsigned long)v[3] << 24; }

static bool printable(const uint8_t *v, size_t n)
{
    if (!n) return false;
    size_t k = n;
    while (k && !v[k - 1]) k--;                    /* trailing NULs are padding */
    if (!k) return false;
    for (size_t i = 0; i < k; i++) if (v[i] < 32 || v[i] > 126) return false;
    return true;
}
static void text(const uint8_t *v, size_t n, char *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap && v[i]; i++) out[k++] = (char)v[i];
    out[k] = 0;
}
/* IEEE 11073 32-bit FLOAT: 24-bit mantissa, 8-bit exponent of ten. */
static float sfloat32(const uint8_t *v)
{
    int32_t m = (int32_t)(v[0] | v[1] << 8 | v[2] << 16);
    if (m & 0x800000) m -= 0x1000000;
    int e = (int8_t)v[3];
    float f = (float)m;
    for (; e > 0; e--) f *= 10.0f;
    for (; e < 0; e++) f /= 10.0f;
    return f;
}

static const char *appearance(unsigned a)
{
    static const char *const cat[] = {"Unknown", "Phone", "Computer", "Watch", "Clock", "Display", "Remote Control",
                                      "Eyeglasses", "Tag", "Keyring", "Media Player", "Barcode Scanner", "Thermometer",
                                      "Heart Rate Sensor", "Blood Pressure", "HID", "Glucose Meter", "Running Sensor",
                                      "Cycling"};
    const unsigned c = a >> 6;
    return c < sizeof(cat) / sizeof(cat[0]) ? cat[c] : "other";
}

bool bx_decode(uint16_t id, const uint8_t *v, size_t n, char *out, size_t cap)
{
    out[0] = 0;
    switch (id) {
    case 0x2A19: if (n >= 1) { snprintf(out, cap, "%u %%", v[0]); return true; } break;
    case 0x2A00: case 0x2A24: case 0x2A25: case 0x2A26: case 0x2A27: case 0x2A28: case 0x2A29:
        if (n) { text(v, n, out, cap); return true; }
        break;
    case 0x2A01: if (n >= 2) { snprintf(out, cap, "%s (0x%04x)", appearance(u16(v)), u16(v)); return true; } break;
    case 0x2A04:
        if (n >= 8) {
            snprintf(out, cap, "interval %u-%u ms, latency %u, timeout %u ms", u16(v) * 5 / 4, u16(v + 2) * 5 / 4, u16(v + 4),
                     u16(v + 6) * 10);
            return true;
        }
        break;
    case 0x2A05: if (n >= 4) { snprintf(out, cap, "handles %u-%u", u16(v), u16(v + 2)); return true; } break;
    case 0x2A06: if (n >= 1) { snprintf(out, cap, "%s", v[0] == 0 ? "no alert" : v[0] == 1 ? "mild" : v[0] == 2 ? "high" : "?"); return true; } break;
    case 0x2A07: if (n >= 1) { snprintf(out, cap, "%d dBm", (int8_t)v[0]); return true; } break;
    case 0x2A2B:
        if (n >= 7) { snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u", u16(v), v[2], v[3], v[4], v[5], v[6]); return true; }
        break;
    case 0x2A37:
        if (n >= 2) {
            const uint8_t f = v[0];
            size_t at = 1;
            unsigned bpm = v[at++];
            if (f & 1) { if (n < 3) break; bpm |= (unsigned)v[at++] << 8; }
            int p = snprintf(out, cap, "%u bpm", bpm);
            if (f & 4) p += snprintf(out + p, cap > (size_t)p ? cap - (size_t)p : 0, f & 2 ? ", contact" : ", no contact");
            if ((f & 8) && at + 2 <= n) { p += snprintf(out + p, cap > (size_t)p ? cap - (size_t)p : 0, ", %u kJ", u16(v + at)); at += 2; }
            if ((f & 16) && at + 2 <= n) snprintf(out + p, cap > (size_t)p ? cap - (size_t)p : 0, ", RR %u ms", u16(v + at) * 1000u / 1024u);
            return true;
        }
        break;
    case 0x2A38: {
        static const char *const where[] = {"other", "chest", "wrist", "finger", "hand", "ear lobe", "foot"};
        if (n >= 1) { snprintf(out, cap, "%s", v[0] < 7 ? where[v[0]] : "?"); return true; }
        break;
    }
    case 0x2A1C:
        if (n >= 5) { snprintf(out, cap, "%.2f %s", (double)sfloat32(v + 1), v[0] & 1 ? "F" : "C"); return true; }
        break;
    case 0x2A6E: if (n >= 2) { snprintf(out, cap, "%.2f C", (double)(int16_t)u16(v) / 100.0); return true; } break;
    case 0x2A6F: if (n >= 2) { snprintf(out, cap, "%.2f %%", (double)u16(v) / 100.0); return true; } break;
    case 0x2A6D: if (n >= 4) { snprintf(out, cap, "%.1f hPa", (double)u32(v) / 1000.0); return true; } break;
    case 0x2A50:
        if (n >= 7) {
            snprintf(out, cap, "%s vendor 0x%04x product 0x%04x version 0x%04x", v[0] == 1 ? "SIG" : v[0] == 2 ? "USB" : "?",
                     u16(v + 1), u16(v + 3), u16(v + 5));
            return true;
        }
        break;
    case 0x2A4E: if (n >= 1) { snprintf(out, cap, "%s protocol", v[0] ? "report" : "boot"); return true; } break;
    case 0x2A5B:
        if (n >= 1) {
            size_t at = 1;
            int p = 0;
            if ((v[0] & 1) && at + 6 <= n) { p += snprintf(out, cap, "wheel %lu revs", u32(v + at)); at += 6; }
            if ((v[0] & 2) && at + 4 <= n) snprintf(out + p, cap > (size_t)p ? cap - (size_t)p : 0, "%scrank %u revs", p ? ", " : "", u16(v + at));
            return out[0] != 0;
        }
        break;
    default: break;
    }
    if (printable(v, n)) {
        char t[BX_VAL + 1];
        text(v, n, t, sizeof(t));
        snprintf(out, cap, "\"%s\"", t);
        return true;
    }
    return false;
}

bool bx_decode_desc(uint16_t id, const uint8_t *v, size_t n, char *out, size_t cap)
{
    out[0] = 0;
    switch (id) {
    case 0x2902:
        if (n >= 2) {
            const unsigned b = u16(v);
            snprintf(out, cap, "%s", (b & 3) == 3 ? "notify+indicate on" : b & 1 ? "notifications on" : b & 2 ? "indications on" : "off");
            return true;
        }
        break;
    case 0x2901: if (n) { text(v, n, out, cap); return true; } break;
    case 0x2900: if (n >= 2) { snprintf(out, cap, "%s%s", v[0] & 1 ? "reliable write " : "", v[0] & 2 ? "writable aux" : ""); return true; } break;
    case 0x2904:
        if (n >= 7) { snprintf(out, cap, "format 0x%02x exp %d unit 0x%04x", v[0], (int8_t)v[1], u16(v + 2)); return true; }
        break;
    case 0x2908:
        if (n >= 2) { snprintf(out, cap, "report %u %s", v[0], v[1] == 1 ? "input" : v[1] == 2 ? "output" : v[1] == 3 ? "feature" : "?"); return true; }
        break;
    default: break;
    }
    return false;
}

/* ---- JSON ----------------------------------------------------------------------------------- */

typedef struct { char *o; size_t cap, n; } jw_t;
static void jw(jw_t *j, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const size_t room = j->n < j->cap ? j->cap - j->n : 0;
    const int k = vsnprintf(room ? j->o + j->n : NULL, room, fmt, ap);
    va_end(ap);
    if (k > 0) j->n += (size_t)k;
}
static void jstr(jw_t *j, const char *s)
{
    jw(j, "\"");
    for (; *s; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') jw(j, "\\%c", ch);
        else if (ch < 32 || ch > 126) jw(j, "\\u%04x", ch);
        else jw(j, "%c", ch);
    }
    jw(j, "\"");
}
static void jvalue(jw_t *j, uint16_t id, bool desc, const uint8_t *v, size_t n, bool has, int32_t err)
{
    char hex[3 * BX_VAL + 1], dec[96];
    if (!has) { jw(j, ", \"value_hex\": null"); }
    else {
        bx_hex(v, n, hex, sizeof(hex));
        jw(j, ", \"value_hex\": \"%s\"", hex);
        if (desc ? bx_decode_desc(id, v, n, dec, sizeof(dec)) : bx_decode(id, v, n, dec, sizeof(dec))) { jw(j, ", \"decoded\": "); jstr(j, dec); }
    }
    if (err) { jw(j, ", \"error\": "); jstr(j, bx_error_text(err)); }
}

size_t bx_json(const bx_conn_t *c, const bx_note_t *notes, int nnotes, int64_t unix_time, char *out, size_t cap)
{
    jw_t j = {out, cap, 0};
    char u[40], a[18];
    snprintf(a, sizeof(a), "%02x:%02x:%02x:%02x:%02x:%02x", c->addr[5], c->addr[4], c->addr[3], c->addr[2], c->addr[1], c->addr[0]);
    jw(&j, "{\n  \"tool\": \"LakeShark BLE EXPLORER\",\n  \"address\": \"%s\",\n  \"address_type\": \"%s\",\n  \"name\": ", a,
       c->addr_type & 1 ? "random" : "public");
    jstr(&j, c->name);
    jw(&j, ",\n  \"saved_unix\": %ld,\n  \"complete\": %s,\n  \"services\": [", (long)unix_time,
       c->phase == BX_P_DONE && !c->truncated ? "true" : "false");
    for (int s = 0; s < c->nsvc; s++) {
        const bx_svc_t *sv = &c->svc[s];
        const char *sn = bx_service_name(&sv->uuid);
        bx_uuid_text(&sv->uuid, u, sizeof(u));
        jw(&j, "%s\n    {\"uuid\": \"%s\", \"name\": ", s ? "," : "", u);
        if (sn) jstr(&j, sn); else jw(&j, "null");
        jw(&j, ", \"start\": %u, \"end\": %u, \"characteristics\": [", sv->start, sv->end);
        int first = 1;
        for (int i = 0; i < c->nchr; i++) {
            const bx_chr_t *h = &c->chr[i];
            if (h->svc != s) continue;
            char props[8];
            const char *cn = bx_char_name(&h->uuid);
            bx_uuid_text(&h->uuid, u, sizeof(u));
            bx_props_text(h->props, props, sizeof(props));
            jw(&j, "%s\n      {\"uuid\": \"%s\", \"name\": ", first ? "" : ",", u);
            first = 0;
            if (cn) jstr(&j, cn); else jw(&j, "null");
            jw(&j, ", \"declaration\": %u, \"value_handle\": %u, \"end\": %u, \"properties\": \"%s\", \"properties_raw\": %u", h->decl,
               h->value, h->end, props, h->props);
            jvalue(&j, bx_uuid16(&h->uuid), false, h->val, h->vlen, h->has_value, h->error);
            if (h->notes) jw(&j, ", \"notifications\": %lu", (unsigned long)h->notes);
            jw(&j, ", \"descriptors\": [");
            int df = 1;
            for (int k = 0; k < c->ndsc; k++) {
                const bx_dsc_t *d = &c->dsc[k];
                if (d->chr != i) continue;
                const char *dn = bx_desc_name(&d->uuid);
                bx_uuid_text(&d->uuid, u, sizeof(u));
                jw(&j, "%s{\"uuid\": \"%s\", \"name\": ", df ? "" : ", ", u);
                df = 0;
                if (dn) jstr(&j, dn); else jw(&j, "null");
                jw(&j, ", \"handle\": %u", d->handle);
                jvalue(&j, bx_uuid16(&d->uuid), true, d->val, d->vlen, d->has_value, d->error);
                jw(&j, "}");
            }
            jw(&j, "]}");
        }
        jw(&j, "\n    ]}");
    }
    jw(&j, "\n  ],\n  \"notifications\": [");
    int nf = 1;
    for (int i = 0; i < nnotes; i++) {
        char hex[3 * BX_VAL + 1];
        bx_hex(notes[i].val, notes[i].vlen, hex, sizeof(hex));
        jw(&j, "%s\n    {\"ms\": %lu, \"handle\": %u, \"value_hex\": \"%s\"}", nf ? "" : ",", (unsigned long)notes[i].at_ms,
           notes[i].handle, hex);
        nf = 0;
    }
    jw(&j, "\n  ]\n}\n");
    return j.n;
}
