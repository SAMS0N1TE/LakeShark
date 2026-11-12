/* See mt_proto.h. */
#include "mt_proto.h"
#include "mt_aes.h"
#include <stdio.h>
#include <string.h>

/* Region bands (kHz). Argentina uses ANZ (915-928). */
const mt_region_t MT_REGIONS[] = {
    { "US",     902000, 928000 },
    { "ANZ",    915000, 928000 },
    { "EU_868", 869400, 869650 },
    { "EU_433", 433000, 434000 },
    { "NZ_865", 864000, 868000 },
    { "IN",     865000, 867000 },
    { "KR",     920000, 923000 },
    { "SG_923", 917000, 925000 },
};
const int MT_REGION_COUNT = (int)(sizeof(MT_REGIONS) / sizeof(MT_REGIONS[0]));

const mt_preset_t MT_PRESETS[] = {
    { "LongFast",     250000, 11, 5 },
    { "MediumFast",   250000,  9, 5 },
    { "MediumSlow",   250000, 10, 5 },
    { "LongModerate", 125000, 11, 8 },
    { "LongSlow",     125000, 12, 8 },
    { "ShortFast",    250000,  7, 5 },
    { "ShortSlow",    250000,  8, 5 },
    { "ShortTurbo",   500000,  7, 5 },
};
const int MT_PRESET_COUNT = (int)(sizeof(MT_PRESETS) / sizeof(MT_PRESETS[0]));

static uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = h * 33u + (uint8_t)*s++;
    return h;
}

uint64_t mt_frequency_hz(int region, int preset, const char *channel_name, int *slot, int *slots)
{
    if (region < 0 || region >= MT_REGION_COUNT) region = 0;
    if (preset < 0 || preset >= MT_PRESET_COUNT) preset = 0;
    const mt_region_t *r = &MT_REGIONS[region];
    const uint32_t bw_khz = MT_PRESETS[preset].bw_hz / 1000u;
    uint32_t n = (r->hi_khz - r->lo_khz) / bw_khz;
    if (n == 0) n = 1;
    const char *name = channel_name && *channel_name ? channel_name : MT_PRESETS[preset].name;
    const uint32_t s = djb2(name) % n;
    if (slot) *slot = (int)s + 1;
    if (slots) *slots = (int)n;
    /* centre of the slot: start + bw/2 + slot * bw */
    return (uint64_t)r->lo_khz * 1000u + MT_PRESETS[preset].bw_hz / 2u + (uint64_t)s * MT_PRESETS[preset].bw_hz;
}

static const uint8_t DEFAULT_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

int mt_channel_key(const uint8_t *psk, int psk_len, uint8_t key[32])
{
    if (psk_len == 0) return 0;
    if (psk_len == 1) {
        if (psk[0] == 0) return 0;               /* "none" */
        memcpy(key, DEFAULT_KEY, 16);
        key[15] = (uint8_t)(key[15] + psk[0] - 1);
        return 16;
    }
    if (psk_len == 16 || psk_len == 32) { memcpy(key, psk, (size_t)psk_len); return psk_len; }
    if (psk_len < 16) { memset(key, 0, 16); memcpy(key, psk, (size_t)psk_len); return 16; }
    memset(key, 0, 32); memcpy(key, psk, (size_t)(psk_len > 32 ? 32 : psk_len)); return 32;
}

uint8_t mt_channel_hash(const char *name, const uint8_t *key, int key_len)
{
    uint8_t h = 0;
    for (const char *p = name; p && *p; p++) h ^= (uint8_t)*p;
    for (int i = 0; i < key_len; i++) h ^= key[i];
    return h;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void mt_header_pack(const mt_header_t *h, uint8_t out[MT_HEADER_LEN])
{
    put32(out, h->dest);
    put32(out + 4, h->from);
    put32(out + 8, h->id);
    out[12] = (uint8_t)((h->hop_limit & 7) | (h->want_ack ? 8 : 0) | (h->via_mqtt ? 16 : 0) | ((h->hop_start & 7) << 5));
    out[13] = h->channel;
    out[14] = h->next_hop;
    out[15] = h->relay;
}

bool mt_header_unpack(const uint8_t *in, size_t n, mt_header_t *h)
{
    if (n < MT_HEADER_LEN) return false;
    h->dest = get32(in);
    h->from = get32(in + 4);
    h->id = get32(in + 8);
    h->hop_limit = in[12] & 7;
    h->want_ack = (in[12] >> 3) & 1;
    h->via_mqtt = (in[12] >> 4) & 1;
    h->hop_start = (in[12] >> 5) & 7;
    h->channel = in[13];
    h->next_hop = in[14];
    h->relay = in[15];
    return true;
}

void mt_crypt(const uint8_t *key, int key_len, uint32_t from, uint32_t id, uint8_t *buf, size_t n)
{
    if (key_len != 16 && key_len != 32) return;
    /* nonce: packet id as 64-bit little-endian, sender as 32-bit little-endian, 4 zero bytes */
    uint8_t iv[16] = {0};
    put32(iv, id);
    put32(iv + 8, from);
    mt_aes_t a;
    mt_aes_setkey(&a, key, key_len);
    mt_aes_ctr(&a, iv, buf, n);
}

/* ---- protobuf: just what these messages use -------------------------- */

static int put_varint(uint8_t *out, size_t cap, size_t at, uint64_t v)
{
    do {
        if (at >= cap) return -1;
        uint8_t b = v & 0x7f;
        v >>= 7;
        out[at++] = (uint8_t)(b | (v ? 0x80 : 0));
    } while (v);
    return (int)at;
}

static int put_bytes(uint8_t *out, size_t cap, size_t at, int field, const void *data, size_t len)
{
    int p = put_varint(out, cap, at, (uint64_t)(field << 3 | 2));
    if (p < 0) return -1;
    p = put_varint(out, cap, (size_t)p, len);
    if (p < 0 || (size_t)p + len > cap) return -1;
    memcpy(out + p, data, len);
    return p + (int)len;
}

static int put_uint(uint8_t *out, size_t cap, size_t at, int field, uint64_t v)
{
    int p = put_varint(out, cap, at, (uint64_t)(field << 3));
    return p < 0 ? -1 : put_varint(out, cap, (size_t)p, v);
}

static bool get_varint(const uint8_t *in, size_t n, size_t *at, uint64_t *v)
{
    *v = 0;
    for (int shift = 0; shift < 64 && *at < n; shift += 7) {
        const uint8_t b = in[(*at)++];
        *v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

/* Walks fields; calls back with (field, wire type, varint value or data). */
typedef void (*field_fn)(void *ctx, int field, int wt, uint64_t v, const uint8_t *data, size_t len);
static bool walk(const uint8_t *in, size_t n, field_fn fn, void *ctx)
{
    size_t at = 0;
    while (at < n) {
        uint64_t key, v = 0;
        if (!get_varint(in, n, &at, &key)) return false;
        const int field = (int)(key >> 3), wt = (int)(key & 7);
        const uint8_t *data = NULL;
        size_t len = 0;
        switch (wt) {
        case 0: if (!get_varint(in, n, &at, &v)) return false; break;
        case 1: if (at + 8 > n) return false; data = in + at; len = 8; at += 8; break;
        case 2: if (!get_varint(in, n, &at, &v) || v > n - at) return false; data = in + at; len = (size_t)v; at += len; break;
        case 5: if (at + 4 > n) return false; data = in + at; len = 4; v = get32(in + at); at += 4; break;
        default: return false;
        }
        fn(ctx, field, wt, v, data, len);
    }
    return true;
}

static int put_fixed32(uint8_t *out, size_t cap, size_t at, int field, uint32_t v)
{
    int p = put_varint(out, cap, at, (uint64_t)(field << 3 | 5));
    if (p < 0 || (size_t)p + 4 > cap) return -1;
    put32(out + p, v);
    return p + 4;
}

int mt_data_encode(const mt_data_t *d, uint8_t *out, size_t cap)
{
    int p = put_uint(out, cap, 0, 1, d->portnum);
    if (p >= 0) p = put_bytes(out, cap, (size_t)p, 2, d->payload, d->payload_len);
    if (p >= 0 && d->want_response) p = put_uint(out, cap, (size_t)p, 3, 1);
    if (p >= 0 && d->request_id) p = put_fixed32(out, cap, (size_t)p, 6, d->request_id);
    return p;
}

static void data_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_data_t *d = ctx;
    if (f == 1 && wt == 0) d->portnum = (uint32_t)v;
    else if (f == 2 && wt == 2) { d->payload = data; d->payload_len = len; }
    else if (f == 3 && wt == 0) d->want_response = v != 0;
    else if (f == 6 && wt == 5) d->request_id = (uint32_t)v;
}

bool mt_data_decode(const uint8_t *in, size_t n, mt_data_t *d)
{
    memset(d, 0, sizeof(*d));
    return walk(in, n, data_field, d) && d->portnum != 0;
}

int mt_user_encode(const mt_user_t *u, const uint8_t mac[6], uint8_t *out, size_t cap)
{
    int p = put_bytes(out, cap, 0, 1, u->id, strlen(u->id));
    if (p >= 0) p = put_bytes(out, cap, (size_t)p, 2, u->long_name, strlen(u->long_name));
    if (p >= 0) p = put_bytes(out, cap, (size_t)p, 3, u->short_name, strlen(u->short_name));
    if (p >= 0) p = put_bytes(out, cap, (size_t)p, 4, mac, 6);
    if (p >= 0) p = put_uint(out, cap, (size_t)p, 5, u->hw_model);
    if (p >= 0 && u->role) p = put_uint(out, cap, (size_t)p, 7, u->role);
    if (p >= 0 && u->has_key) p = put_bytes(out, cap, (size_t)p, 8, u->public_key, 32);
    return p;
}

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    size_t j = 0;
    for (size_t i = 0; i < len && j + 1 < cap; i++)
        dst[j++] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : (src[i] >= 0x80 ? '?' : ' ');
    dst[j] = 0;
}

static void user_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_user_t *u = ctx;
    if (wt == 2 && f == 1) copy_str(u->id, sizeof(u->id), data, len);
    else if (wt == 2 && f == 2) copy_str(u->long_name, sizeof(u->long_name), data, len);
    else if (wt == 2 && f == 3) copy_str(u->short_name, sizeof(u->short_name), data, len);
    else if (wt == 0 && f == 5) u->hw_model = (uint32_t)v;
    else if (wt == 0 && f == 7) u->role = (uint32_t)v;
    else if (wt == 2 && f == 8 && len == 32) { memcpy(u->public_key, data, 32); u->has_key = true; }
}

bool mt_user_decode(const uint8_t *in, size_t n, mt_user_t *u)
{
    memset(u, 0, sizeof(*u));
    return walk(in, n, user_field, u);
}

static void pos_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_position_t *p = ctx;
    (void)data; (void)len;
    if (f == 1 && wt == 5) { p->lat = (int32_t)(uint32_t)v * 1e-7; p->has_pos = true; }
    else if (f == 2 && wt == 5) p->lon = (int32_t)(uint32_t)v * 1e-7;
    else if (f == 3 && wt == 0) { p->alt = (int32_t)v; p->has_alt = true; }
    else if (f == 4 && wt == 5) p->time = (uint32_t)v;
}

bool mt_position_decode(const uint8_t *in, size_t n, mt_position_t *p)
{
    memset(p, 0, sizeof(*p));
    return walk(in, n, pos_field, p) && p->has_pos;
}

int mt_position_encode(double lat, double lon, int32_t alt, uint32_t time, int precision_bits,
                       uint8_t *out, size_t cap)
{
    int32_t li = (int32_t)(lat * 1e7), lo = (int32_t)(lon * 1e7);
    if (precision_bits > 0 && precision_bits < 32) {
        /* keep the top bits, centre in the cell so the spot is not biased */
        const uint32_t mask = 0xFFFFFFFFu << (32 - precision_bits), half = 1u << (31 - precision_bits);
        li = (int32_t)(((uint32_t)li & mask) + half);
        lo = (int32_t)(((uint32_t)lo & mask) + half);
    }
    int p = put_fixed32(out, cap, 0, 1, (uint32_t)li);
    if (p >= 0) p = put_fixed32(out, cap, (size_t)p, 2, (uint32_t)lo);
    if (p >= 0 && alt) p = put_uint(out, cap, (size_t)p, 3, (uint64_t)(int64_t)alt);
    if (p >= 0 && time) p = put_fixed32(out, cap, (size_t)p, 4, time);
    if (p >= 0 && precision_bits > 0 && precision_bits < 32) p = put_uint(out, cap, (size_t)p, 23, (uint32_t)precision_bits);
    return p;
}

static float as_float(uint64_t v) { const uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }

static void device_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_device_t *d = ctx;
    (void)data; (void)len;
    if (f == 1 && wt == 0) { d->battery = (uint32_t)v; d->has = true; }
    else if (f == 2 && wt == 5) { d->voltage = as_float(v); d->has = true; }
    else if (f == 3 && wt == 5) d->ch_util = as_float(v);
    else if (f == 4 && wt == 5) d->air_util = as_float(v);
    else if (f == 5 && wt == 0) d->uptime = (uint32_t)v;
}

typedef struct { mt_device_t *d; bool ok; } telem_ctx_t;
static void telem_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    telem_ctx_t *t = ctx;
    (void)v;
    if (f == 2 && wt == 2) t->ok = walk(data, len, device_field, t->d);
}

bool mt_telemetry_decode(const uint8_t *in, size_t n, mt_device_t *d)
{
    memset(d, 0, sizeof(*d));
    telem_ctx_t t = { d, false };
    return walk(in, n, telem_field, &t) && t.ok && d->has;
}

static int put_float(uint8_t *out, size_t cap, size_t at, int field, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return put_fixed32(out, cap, at, field, u);
}

int mt_telemetry_encode(uint32_t time, const mt_device_t *d, uint8_t *out, size_t cap)
{
    uint8_t m[40];
    int q = 0;
    if (d->battery) q = put_uint(m, sizeof(m), (size_t)q, 1, d->battery);
    if (q >= 0 && d->voltage > 0) q = put_float(m, sizeof(m), (size_t)q, 2, d->voltage);
    if (q >= 0) q = put_float(m, sizeof(m), (size_t)q, 3, d->ch_util);
    if (q >= 0) q = put_float(m, sizeof(m), (size_t)q, 4, d->air_util);
    if (q >= 0) q = put_uint(m, sizeof(m), (size_t)q, 5, d->uptime);
    if (q < 0) return -1;
    int p = 0;
    if (time) p = put_fixed32(out, cap, 0, 1, time);
    return p < 0 ? -1 : put_bytes(out, cap, (size_t)p, 2, m, (size_t)q);
}

/* Repeated fields go packed (proto3); both forms are read. */
static int put_packed_fixed32(uint8_t *out, size_t cap, size_t at, int field, const uint32_t *v, int n)
{
    if (!n) return (int)at;
    uint8_t tmp[MT_ROUTE_MAX * 4];
    for (int i = 0; i < n; i++) put32(tmp + 4 * i, v[i]);
    return put_bytes(out, cap, at, field, tmp, (size_t)n * 4);
}
static int put_packed_int(uint8_t *out, size_t cap, size_t at, int field, const int8_t *v, int n)
{
    if (!n) return (int)at;
    uint8_t tmp[(MT_ROUTE_MAX + 1) * 10];
    int q = 0;
    for (int i = 0; i < n && q >= 0; i++) q = put_varint(tmp, sizeof(tmp), (size_t)q, (uint64_t)(int64_t)v[i]);
    return q < 0 ? -1 : put_bytes(out, cap, at, field, tmp, (size_t)q);
}

int mt_route_encode(const mt_route_t *r, uint8_t *out, size_t cap)
{
    int p = put_packed_fixed32(out, cap, 0, 1, r->route, r->n_route);
    if (p >= 0) p = put_packed_int(out, cap, (size_t)p, 2, r->snr_to, r->n_snr_to);
    if (p >= 0) p = put_packed_fixed32(out, cap, (size_t)p, 3, r->back, r->n_back);
    if (p >= 0) p = put_packed_int(out, cap, (size_t)p, 4, r->snr_back, r->n_snr_back);
    return p;
}

static void route_add_u32(uint32_t *list, uint8_t *n, uint32_t v) { if (*n < MT_ROUTE_MAX) list[(*n)++] = v; }
static void route_add_snr(int8_t *list, uint8_t *n, int64_t v)
{
    if (*n < MT_ROUTE_MAX + 1) list[(*n)++] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
}

static void route_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_route_t *r = ctx;
    uint32_t *ids = f == 1 ? r->route : f == 3 ? r->back : NULL;
    uint8_t *nid = f == 1 ? &r->n_route : &r->n_back;
    int8_t *snr = f == 2 ? r->snr_to : f == 4 ? r->snr_back : NULL;
    uint8_t *nsnr = f == 2 ? &r->n_snr_to : &r->n_snr_back;
    if (ids && wt == 5) route_add_u32(ids, nid, (uint32_t)v);
    else if (ids && wt == 2) for (size_t i = 0; i + 4 <= len; i += 4) route_add_u32(ids, nid, get32(data + i));
    else if (snr && wt == 0) route_add_snr(snr, nsnr, (int64_t)v);
    else if (snr && wt == 2) {
        size_t at = 0;
        uint64_t x;
        while (at < len && get_varint(data, len, &at, &x)) route_add_snr(snr, nsnr, (int64_t)x);
    }
}

bool mt_route_decode(const uint8_t *in, size_t n, mt_route_t *r)
{
    memset(r, 0, sizeof(*r));
    return walk(in, n, route_field, r);
}

static void routing_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    (void)data; (void)len;
    if (f == 3 && wt == 0) *(uint32_t *)ctx = (uint32_t)v;
}

bool mt_routing_decode(const uint8_t *in, size_t n, uint32_t *error)
{
    *error = 0;
    return walk(in, n, routing_field, error);
}

int mt_routing_ack(uint8_t *out, size_t cap)
{
    return put_uint(out, cap, 0, 3, 0);     /* error_reason NONE */
}

const char *mt_port_name(uint32_t port)
{
    switch (port) {
    case MT_PORT_TEXT: return "TEXT";
    case MT_PORT_POSITION: return "POSITION";
    case MT_PORT_NODEINFO: return "NODEINFO";
    case MT_PORT_ROUTING: return "ROUTING";
    case MT_PORT_TELEMETRY: return "TELEMETRY";
    case MT_PORT_TRACEROUTE: return "TRACEROUTE";
    case MT_PORT_NEIGHBORINFO: return "NEIGHBORS";
    default: return "OTHER";
    }
}
