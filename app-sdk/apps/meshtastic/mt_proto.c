/* See mt_proto.h. Source-derived from Meshtastic's protobufs and firmware
   (Copyright (C) Meshtastic LLC and contributors, GPL-3.0): field numbers,
   enum values, the slot rule, preset names, module behaviour. GPL-3.0. */
#include "mt_proto.h"
#include "mt_aes.h"
#include "mt_pki.h"
#include <stdio.h>
#include <string.h>

/* Region bands (kHz), duty cycle, power limit (dBm), RegionCode. The first
   eight keep the order older versions saved. Argentina uses ANZ (915-928). */
const mt_region_t MT_REGIONS[] = {
    { "US",      902000,  928000, 100, 30, 0, 1 },
    { "ANZ",     915000,  928000, 100, 30, 0, 6 },
    { "EU_868",  869400,  869650,  10, 27, 0, 3 },
    { "EU_433",  433000,  434000,  10, 10, 0, 2 },
    { "NZ_865",  864000,  868000, 100, 36, 0, 11 },
    { "IN",      865000,  867000, 100, 30, 0, 10 },
    { "KR",      920000,  923000, 100, 23, 0, 7 },
    { "SG_923",  917000,  925000, 100, 20, 0, 18 },
    { "CN",      470000,  510000, 100, 19, 0, 4 },
    { "JP",      920500,  923500, 100, 13, 0, 5 },
    { "ANZ_433", 433050,  434790, 100, 14, 0, 22 },
    { "RU",      868700,  869200, 100, 20, 0, 9 },
    { "TW",      920000,  925000, 100, 27, 0, 8 },
    { "TH",      920000,  925000,  10, 27, 0, 12 },
    { "UA_433",  433000,  434700,  10, 10, 0, 14 },
    { "UA_868",  868000,  868600,   1, 14, 0, 15 },
    { "MY_433",  433000,  435000, 100, 20, 0, 16 },
    { "MY_919",  919000,  924000, 100, 27, 0, 17 },
    { "PH_433",  433000,  434700, 100, 10, 0, 19 },
    { "PH_868",  868000,  869400, 100, 14, 0, 20 },
    { "PH_915",  915000,  918000, 100, 24, 0, 21 },
    { "KZ_433",  433075,  434775, 100, 10, 0, 23 },
    { "KZ_863",  863000,  868000, 100, 30, 0, 24 },
    { "NP_865",  865000,  868000, 100, 30, 0, 25 },
    { "BR_902",  902000,  907500, 100, 30, 0, 26 },
    { "LORA_24", 2400000, 2483500, 100, 10, 1, 13 },
};
const int MT_REGION_COUNT = (int)(sizeof(MT_REGIONS) / sizeof(MT_REGIONS[0]));

const mt_preset_t MT_PRESETS[] = {
    { "LongFast",     250000, 11, 5, 0 },
    { "MediumFast",   250000,  9, 5, 4 },
    { "MediumSlow",   250000, 10, 5, 3 },
    { "LongMod",      125000, 11, 8, 7 },
    { "LongSlow",     125000, 12, 8, 1 },
    { "ShortFast",    250000,  7, 5, 6 },
    { "ShortSlow",    250000,  8, 5, 5 },
    { "ShortTurbo",   500000,  7, 5, 8 },
};
const int MT_PRESET_COUNT = (int)(sizeof(MT_PRESETS) / sizeof(MT_PRESETS[0]));

static uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = h * 33u + (uint8_t)*s++;
    return h;
}

int mt_region_by_code(uint32_t code)
{
    for (int i = 0; i < MT_REGION_COUNT; i++) if (MT_REGIONS[i].code == code) return i;
    return -1;
}
int mt_preset_by_code(uint32_t code)
{
    for (int i = 0; i < MT_PRESET_COUNT; i++) if (MT_PRESETS[i].code == code) return i;
    return -1;
}

/* Bandwidth codes as LoRaConfig.bandwidth has them (kHz; 31 = 31.25, 62 = 62.5). */
static const uint32_t BW_HZ[MT_BW_COUNT] = { 31250, 62500, 125000, 250000, 500000 };

uint32_t mt_lora_bw_hz(const mt_lora_t *l)
{
    const int region = l->region < MT_REGION_COUNT ? l->region : 0;
    if (!l->custom) return mt_bw_hz(region, l->preset);
    const uint32_t bw = BW_HZ[l->bw_code < MT_BW_COUNT ? l->bw_code : MT_BW_250];
    if (!MT_REGIONS[region].wide) return bw;
    return bw < 125000 ? 203125 : bw / 4u * 13u;   /* 2.4 GHz: 203.125 .. 1625 kHz */
}
void mt_lora_sf_cr(const mt_lora_t *l, uint8_t *sf, uint8_t *cr)
{
    const mt_preset_t *p = &MT_PRESETS[l->preset < MT_PRESET_COUNT ? l->preset : 0];
    *sf = l->custom ? (l->sf >= 5 && l->sf <= 12 ? l->sf : 11) : p->sf;
    *cr = l->custom ? (l->cr >= 5 && l->cr <= 8 ? l->cr : 5) : p->cr;
}
const char *mt_lora_name(const mt_lora_t *l)
{
    return l->custom ? "Custom" : MT_PRESETS[l->preset < MT_PRESET_COUNT ? l->preset : 0].name;
}

uint64_t mt_lora_frequency(const mt_lora_t *l, const char *channel_name, int *slot, int *slots)
{
    const mt_region_t *r = &MT_REGIONS[l->region < MT_REGION_COUNT ? l->region : 0];
    const uint32_t bw = mt_lora_bw_hz(l);
    /* the official count: whole slots of the bandwidth that fit the band */
    const uint32_t n = (uint32_t)(((uint64_t)(r->hi_khz - r->lo_khz) * 1000u) / bw);
    if (slot) *slot = 0;
    if (slots) *slots = (int)n;
    if (n == 0) return 0;                  /* the bandwidth is wider than the band */
    const char *name = channel_name && *channel_name ? channel_name : mt_lora_name(l);
    /* channel_num: 0 = the name's hash picks the slot, else that slot */
    const uint32_t s = (l->slot ? (uint32_t)(l->slot - 1) : djb2(name)) % n;
    if (slot) *slot = (int)s + 1;
    /* centre of the slot: start + bw/2 + slot * bw, plus the offset */
    return (uint64_t)((int64_t)r->lo_khz * 1000 + bw / 2u + (int64_t)s * bw + l->offset_hz);
}

uint64_t mt_frequency_hz(int region, int preset, const char *channel_name, int *slot, int *slots)
{
    if (region < 0 || region >= MT_REGION_COUNT) region = 0;
    if (preset < 0 || preset >= MT_PRESET_COUNT) preset = 0;
    const mt_lora_t l = { (uint8_t)region, (uint8_t)preset, 0, 0, 0, 0, 0, 0 };
    return mt_lora_frequency(&l, channel_name, slot, slots);
}

uint32_t mt_bw_hz(int region, int preset)
{
    if (region < 0 || region >= MT_REGION_COUNT) region = 0;
    if (preset < 0 || preset >= MT_PRESET_COUNT) preset = 0;
    const uint32_t bw = MT_PRESETS[preset].bw_hz;
    return MT_REGIONS[region].wide ? bw / 4u * 13u : bw;      /* 250 -> 812.5 kHz */
}

int mt_region_guess(double lat, double lon)
{
    if (lat > 24 && lat < 50 && lon > 122 && lon < 146) return lon > 129 ? 9 : 6;  /* JP / KR */
    if (lat > 33 && lat < 39 && lon > 124 && lon < 130) return 6;                  /* KR */
    if (lat > 21 && lat < 26 && lon > 119 && lon < 123) return 12;                 /* TW */
    if (lat > 18 && lat < 54 && lon > 73 && lon < 135) return 8;                   /* CN */
    if (lat > 6 && lat < 36 && lon > 68 && lon < 90) return 5;                     /* IN */
    if (lat > 5 && lat < 21 && lon > 97 && lon < 106) return 13;                   /* TH */
    if (lat > 0 && lat < 8 && lon > 99 && lon < 120) return 7;                     /* MY/SG ~ SG_923 */
    if (lat > -48 && lat < -33 && lon > 166 && lon < 179) return 4;                /* NZ */
    if (lat > -45 && lat < -9 && lon > 112 && lon < 155) return 1;                 /* ANZ */
    if (lat > 50 && lat < 75 && lon > 30 && lon < 180) return 11;                  /* RU */
    if (lat > 35 && lat < 72 && lon > -25 && lon < 45) return 2;                   /* Europe */
    if (lat > 12 && lat < 72 && lon > -170 && lon < -50) return 0;                 /* North America */
    if (lat > -34 && lat < 5 && lon > -74 && lon < -34) return 24;                 /* BR */
    if (lat > -56 && lat < -9 && lon > -76 && lon < -50) return 1;                 /* AR, CL, UY: ANZ band */
    return 0;
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
    if (p >= 0 && d->reply_id) p = put_fixed32(out, cap, (size_t)p, 7, d->reply_id);
    if (p >= 0 && d->emoji) p = put_fixed32(out, cap, (size_t)p, 8, d->emoji);
    return p;
}

static void data_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_data_t *d = ctx;
    if (f == 1 && wt == 0) d->portnum = (uint32_t)v;
    else if (f == 2 && wt == 2) { d->payload = data; d->payload_len = len; }
    else if (f == 3 && wt == 0) d->want_response = v != 0;
    else if (f == 6 && wt == 5) d->request_id = (uint32_t)v;
    else if (f == 7 && wt == 5) d->reply_id = (uint32_t)v;
    else if (f == 8 && (wt == 5 || wt == 0)) d->emoji = (uint32_t)v;
}

int mt_text_room(bool reply, bool emoji, int crypto_overhead)
{
    /* portnum (2) + payload key and a two-byte length (3), fixed32 fields 5 each */
    const int room = MT_MAX_PAYLOAD - crypto_overhead - 5 - (reply ? 5 : 0) - (emoji ? 5 : 0);
    return room < MT_DATA_PAYLOAD ? room : MT_DATA_PAYLOAD;
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
    if (p >= 0 && u->licensed) p = put_uint(out, cap, (size_t)p, 6, 1);
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
    else if (wt == 0 && f == 6) u->licensed = v != 0;
    else if (wt == 0 && f == 7) u->role = (uint32_t)v;
    else if (wt == 2 && f == 8 && len == 32) { memcpy(u->public_key, data, 32); u->has_key = true; }
    else if (wt == 0 && f == 9) u->unmessagable = v != 0;
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
    else if (f == 23 && wt == 0) p->precision = (uint8_t)(v > 32 ? 32 : v);
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

/* ---- Waypoint (mesh.proto) ------------------------------------------- */
static int put_str(uint8_t *out, size_t cap, size_t at, int field, const char *s)
{
    return *s ? put_bytes(out, cap, at, field, s, strlen(s)) : (int)at;
}

int mt_waypoint_encode(const mt_waypoint_t *w, uint8_t *out, size_t cap)
{
    int p = put_uint(out, cap, 0, 1, w->id);
    if (p >= 0 && w->has_pos) p = put_fixed32(out, cap, (size_t)p, 2, (uint32_t)(int32_t)(w->lat * 1e7));
    if (p >= 0 && w->has_pos) p = put_fixed32(out, cap, (size_t)p, 3, (uint32_t)(int32_t)(w->lon * 1e7));
    if (p >= 0 && w->expire) p = put_uint(out, cap, (size_t)p, 4, w->expire);
    if (p >= 0 && w->locked_to) p = put_uint(out, cap, (size_t)p, 5, w->locked_to);
    if (p >= 0) p = put_str(out, cap, (size_t)p, 6, w->name);
    if (p >= 0) p = put_str(out, cap, (size_t)p, 7, w->desc);
    if (p >= 0 && w->icon) p = put_fixed32(out, cap, (size_t)p, 8, w->icon);
    return p;
}

static void raw_str(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(dst, src, n);
    while (n > 0 && ((uint8_t)dst[n - 1] & 0xC0) == 0x80) n--;          /* no letter cut in half */
    if (n > 0 && (uint8_t)dst[n - 1] >= 0xC0 && n < len) n--;
    dst[n] = 0;
    for (size_t i = 0; i < n; i++) if ((uint8_t)dst[i] < 32) dst[i] = ' ';
}

static void wpt_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_waypoint_t *w = ctx;
    if (f == 1 && wt == 0) w->id = (uint32_t)v;
    else if (f == 2 && wt == 5) { w->lat = (int32_t)(uint32_t)v * 1e-7; w->has_pos = true; }
    else if (f == 3 && wt == 5) w->lon = (int32_t)(uint32_t)v * 1e-7;
    else if (f == 4 && wt == 0) w->expire = (uint32_t)v;
    else if (f == 5 && wt == 0) w->locked_to = (uint32_t)v;
    else if (f == 6 && wt == 2) raw_str(w->name, sizeof(w->name), data, len);
    else if (f == 7 && wt == 2) raw_str(w->desc, sizeof(w->desc), data, len);
    else if (f == 8 && wt == 5) w->icon = (uint32_t)v;
}

bool mt_waypoint_decode(const uint8_t *in, size_t n, mt_waypoint_t *w)
{
    memset(w, 0, sizeof(*w));
    return walk(in, n, wpt_field, w) && w->id != 0;
}

/* ---- NeighborInfo (mesh.proto) --------------------------------------- */
int mt_neighbors_encode(const mt_neighbors_t *nb, uint8_t *out, size_t cap)
{
    int p = put_uint(out, cap, 0, 1, nb->node);
    if (p >= 0 && nb->last_sent_by) p = put_uint(out, cap, (size_t)p, 2, nb->last_sent_by);
    if (p >= 0 && nb->interval_s) p = put_uint(out, cap, (size_t)p, 3, nb->interval_s);
    for (int i = 0; i < nb->n && i < MT_NBR_MAX && p >= 0; i++) {
        uint8_t m[16];
        int q = put_uint(m, sizeof(m), 0, 1, nb->id[i]);
        if (q >= 0) q = put_float(m, sizeof(m), (size_t)q, 2, nb->snr[i]);
        p = q < 0 ? -1 : put_bytes(out, cap, (size_t)p, 4, m, (size_t)q);
    }
    return p;
}

static void nbr_one(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_neighbors_t *nb = ctx;
    (void)data; (void)len;
    if (f == 1 && wt == 0) nb->id[nb->n] = (uint32_t)v;
    else if (f == 2 && wt == 5) nb->snr[nb->n] = as_float(v);
}
static void nbr_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_neighbors_t *nb = ctx;
    if (f == 1 && wt == 0) nb->node = (uint32_t)v;
    else if (f == 2 && wt == 0) nb->last_sent_by = (uint32_t)v;
    else if (f == 3 && wt == 0) nb->interval_s = (uint32_t)v;
    else if (f == 4 && wt == 2 && nb->n < MT_NBR_MAX) {
        nb->id[nb->n] = 0; nb->snr[nb->n] = 0;
        if (walk(data, len, nbr_one, nb) && nb->id[nb->n]) nb->n++;
    }
}

bool mt_neighbors_decode(const uint8_t *in, size_t n, mt_neighbors_t *nb)
{
    memset(nb, 0, sizeof(*nb));
    return walk(in, n, nbr_field, nb) && nb->node != 0;
}

/* ---- StoreAndForward (storeforward.proto) ----------------------------- */
int mt_sf_history_request(uint32_t minutes, uint8_t *out, size_t cap)
{
    int p = put_uint(out, cap, 0, 1, MT_SF_CLIENT_HISTORY);
    if (p >= 0 && minutes) {
        uint8_t h[8];
        const int q = put_uint(h, sizeof(h), 0, 2, minutes);
        p = q < 0 ? -1 : put_bytes(out, cap, (size_t)p, 3, h, (size_t)q);
    }
    return p;
}

static void sf_history(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_sf_t *sf = ctx;
    (void)data; (void)len;
    if (f == 1 && wt == 0) sf->history_messages = (uint32_t)v;
    else if (f == 2 && wt == 0) sf->window = (uint32_t)v;
}
static void sf_heartbeat(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    (void)data; (void)len;
    if (f == 1 && wt == 0) ((mt_sf_t *)ctx)->period = (uint32_t)v;
}
static void sf_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_sf_t *sf = ctx;
    if (f == 1 && wt == 0) sf->rr = (uint32_t)v;
    else if (f == 3 && wt == 2) walk(data, len, sf_history, sf);
    else if (f == 4 && wt == 2) walk(data, len, sf_heartbeat, sf);
    else if (f == 5 && wt == 2) { sf->text = data; sf->text_len = len; }
}

bool mt_sf_decode(const uint8_t *in, size_t n, mt_sf_t *sf)
{
    memset(sf, 0, sizeof(*sf));
    return walk(in, n, sf_field, sf) && sf->rr != 0;
}

/* ---- ChannelSet (apponly.proto, channel.proto, config.proto) ------------ */
static const uint32_t BW_CODE[MT_BW_COUNT] = { 31, 62, 125, 250, 500 };

int mt_chset_encode(const mt_chset_t *s, uint8_t *out, size_t cap)
{
    int p = 0;
    for (int i = 0; i < s->n && i < MT_SET_MAX && p >= 0; i++) {
        const mt_chset_ch_t *c = &s->ch[i];
        uint8_t b[80], m[12];
        int q = c->psk_len ? put_bytes(b, sizeof(b), 0, 2, c->psk, c->psk_len) : 0;
        if (q >= 0) q = put_str(b, sizeof(b), (size_t)q, 3, c->name);
        if (q >= 0 && (c->precision || c->muted)) {
            int r = c->precision ? put_uint(m, sizeof(m), 0, 1, c->precision) : 0;
            if (r >= 0 && c->muted) r = put_uint(m, sizeof(m), (size_t)r, 2, 1);
            q = r < 0 ? -1 : put_bytes(b, sizeof(b), (size_t)q, 7, m, (size_t)r);
        }
        p = q < 0 ? -1 : put_bytes(out, cap, (size_t)p, 1, b, (size_t)q);
    }
    if (p >= 0 && s->has_lora) {
        const mt_lora_t *l = &s->lora;
        uint8_t b[64];
        int q = 0;
        if (!l->custom) {
            q = put_uint(b, sizeof(b), 0, 1, 1);
            if (q >= 0 && MT_PRESETS[l->preset < MT_PRESET_COUNT ? l->preset : 0].code)
                q = put_uint(b, sizeof(b), (size_t)q, 2, MT_PRESETS[l->preset < MT_PRESET_COUNT ? l->preset : 0].code);
        } else {
            const int wide = MT_REGIONS[l->region < MT_REGION_COUNT ? l->region : 0].wide;
            const uint32_t bw = mt_lora_bw_hz(l);
            const uint32_t code = wide ? (bw < 300000 ? 200 : bw < 600000 ? 400 : bw < 1000000 ? 800 : 1600)
                                       : BW_CODE[l->bw_code < MT_BW_COUNT ? l->bw_code : MT_BW_250];
            q = put_uint(b, sizeof(b), 0, 3, code);
            if (q >= 0) q = put_uint(b, sizeof(b), (size_t)q, 4, l->sf);
            if (q >= 0) q = put_uint(b, sizeof(b), (size_t)q, 5, l->cr);
        }
        if (q >= 0 && l->offset_hz) q = put_float(b, sizeof(b), (size_t)q, 6, (float)l->offset_hz / 1e6f);
        if (q >= 0 && s->region_code) q = put_uint(b, sizeof(b), (size_t)q, 7, s->region_code);
        if (q >= 0 && s->hop_limit) q = put_uint(b, sizeof(b), (size_t)q, 8, s->hop_limit);
        if (q >= 0) q = put_uint(b, sizeof(b), (size_t)q, 9, 1);                 /* tx_enabled */
        if (q >= 0 && l->slot) q = put_uint(b, sizeof(b), (size_t)q, 11, l->slot);
        p = q < 0 ? -1 : put_bytes(out, cap, (size_t)p, 2, b, (size_t)q);
    }
    return p;
}

static void chset_module(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_chset_ch_t *c = ctx;
    (void)data; (void)len;
    if (f == 1 && wt == 0) c->precision = (uint8_t)(v > 32 ? 32 : v);
    else if (f == 2 && wt == 0) c->muted = v != 0;
}
static void chset_one(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    mt_chset_ch_t *c = ctx;
    (void)v;
    if (f == 2 && wt == 2 && len <= 32) { memcpy(c->psk, data, len); c->psk_len = (uint8_t)len; }
    else if (f == 3 && wt == 2) raw_str(c->name, sizeof(c->name), data, len);
    else if (f == 7 && wt == 2) { c->has_module = 1; walk(data, len, chset_module, c); }
}
typedef struct { mt_chset_t *s; uint32_t use_preset, preset_code, bw; } chset_ctx_t;
static void chset_lora(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    chset_ctx_t *k = ctx;
    mt_lora_t *l = &k->s->lora;
    (void)data; (void)len;
    if (wt == 5 && f == 6) { l->offset_hz = (int32_t)(as_float(v) * 1e6f); return; }
    if (wt != 0) return;
    switch (f) {
    case 1: k->use_preset = (uint32_t)v; break;
    case 2: k->preset_code = (uint32_t)v; break;
    case 3: k->bw = (uint32_t)v; break;
    case 4: l->sf = (uint8_t)v; break;
    case 5: l->cr = (uint8_t)v; break;
    case 7: k->s->region_code = (uint32_t)v; break;
    case 8: k->s->hop_limit = (uint32_t)v > 7 ? 7 : (uint32_t)v; break;
    case 11: l->slot = (uint16_t)v; break;
    default: break;
    }
}
static void chset_field(void *ctx, int f, int wt, uint64_t v, const uint8_t *data, size_t len)
{
    chset_ctx_t *k = ctx;
    mt_chset_t *s = k->s;
    (void)v;
    if (f == 1 && wt == 2 && s->n < MT_SET_MAX) {
        memset(&s->ch[s->n], 0, sizeof(s->ch[0]));
        if (walk(data, len, chset_one, &s->ch[s->n])) s->n++;
    } else if (f == 2 && wt == 2) {
        s->has_lora = true;
        walk(data, len, chset_lora, k);
    }
}

bool mt_chset_decode(const uint8_t *in, size_t n, mt_chset_t *s)
{
    memset(s, 0, sizeof(*s));
    chset_ctx_t k = { s, 0, 0, 0 };
    if (!walk(in, n, chset_field, &k) || s->n == 0) return false;
    mt_lora_t *l = &s->lora;
    const int r = mt_region_by_code(s->region_code);
    l->region = (uint8_t)(r < 0 ? 0 : r);
    if (r < 0) s->region_code = 0;
    const int pr = mt_preset_by_code(k.preset_code);
    l->preset = (uint8_t)(pr < 0 ? 0 : pr);
    /* custom only when it says so and names a bandwidth */
    l->custom = !k.use_preset && k.bw ? 1 : 0;
    if (l->custom) {
        const uint32_t bw = k.bw;
        l->bw_code = (uint8_t)(bw <= 31 ? MT_BW_31 : bw <= 62 ? MT_BW_62 : bw <= 125 || bw == 400 ? MT_BW_125
                               : bw <= 250 || bw == 800 ? MT_BW_250 : bw == 200 ? MT_BW_31 : MT_BW_500);
    }
    return true;
}

static const char URL_HEAD[] = "https://meshtastic.org/e/#";

int mt_chset_url(const mt_chset_t *s, char *out, int cap)
{
    uint8_t b[700];
    const int n = mt_chset_encode(s, b, sizeof(b));
    const int head = (int)sizeof(URL_HEAD) - 1;
    if (n < 0 || cap < head + (n + 2) / 3 * 4 + 1) return -1;
    memcpy(out, URL_HEAD, (size_t)head);
    mt_b64_encode(b, n, out + head, cap - head);
    int j = head;                                   /* base64url, unpadded */
    for (int i = head; out[i]; i++) {
        if (out[i] == '=') continue;
        out[j++] = out[i] == '+' ? '-' : out[i] == '/' ? '_' : out[i];
    }
    out[j] = 0;
    return j;
}

bool mt_chset_parse(const char *url, mt_chset_t *s, bool *add)
{
    const char *hash = strchr(url, '#');
    if (add) *add = strstr(url, "add=true") != NULL;
    const char *b = hash ? hash + 1 : url;
    char clean[960];
    int j = 0;
    for (; *b && j < (int)sizeof(clean) - 1; b++)
        if (*b != ' ' && *b != '\n' && *b != '\r') clean[j++] = *b;
    clean[j] = 0;
    char *q = strchr(clean, '?');                   /* a query after the fragment */
    if (q) *q = 0;
    uint8_t raw[720];
    const int n = mt_b64_decode(clean, raw, sizeof(raw));
    return n > 0 && mt_chset_decode(raw, (size_t)n, s);
}

/* ---- names ----------------------------------------------------------- */
const char *mt_role_name(uint32_t role)
{
    static const char *const R[] = { "CLIENT", "CLIENT_MUTE", "ROUTER", "ROUTER_CLIENT", "REPEATER", "TRACKER",
                                     "SENSOR", "TAK", "CLIENT_HIDDEN", "LOST_AND_FOUND", "TAK_TRACKER",
                                     "ROUTER_LATE", "CLIENT_BASE" };
    static char other[16];
    if (role < sizeof(R) / sizeof(R[0])) return R[role];
    snprintf(other, sizeof(other), "ROLE %u", (unsigned)role);
    return other;
}

/* HardwareModel names, by value, '|' between them (mesh.proto). */
static const char HW[] =
    "UNSET|TLORA_V2|TLORA_V1|TLORA_V2_1_1P6|TBEAM|HELTEC_V2_0|TBEAM_V0P7|T_ECHO|TLORA_V1_1P3|RAK4631|HELTEC_V2_1|"
    "HELTEC_V1|TBEAM_S3_CORE|RAK11200|NANO_G1|TLORA_V2_1_1P8|TLORA_T3_S3|NANO_G1_EXPLORER|NANO_G2_ULTRA|LORA_TYPE|"
    "WIPHONE|WIO_WM1110|RAK2560|HELTEC_HRU_3601|HELTEC_WL_BRIDGE|STATION_G1|RAK11310|MAKERFABS_TRACKER|"
    "MAKERFABS_RESERVED|CANARYONE|RP2040_LORA|STATION_G2|LORA_RELAY_V1|T_ECHO_PLUS|PPR|GENIEBLOCKS|NRF52_UNKNOWN|"
    "PORTDUINO|ANDROID_SIM|DIY_V1|NRF52840_PCA10059|DR_DEV|M5STACK|HELTEC_V3|HELTEC_WSL_V3|BETAFPV_2400_TX|"
    "BETAFPV_900_NANO_TX|RPI_PICO|HELTEC_WL_TRACKER|HELTEC_WL_PAPER|T_DECK|T_WATCH_S3|PICOMPUTER_S3|HELTEC_HT62|"
    "EBYTE_ESP32_S3|ESP32_S3_PICO|CHATTER_2|HELTEC_WL_PAPER_V1_0|HELTEC_WL_TRACKER_V1_0|UNPHONE|TD_LORAC|"
    "CDEBYTE_EORA_S3|TWC_MESH_V4|NRF52_PROMICRO_DIY|RM_900_BANDIT_NANO|HELTEC_CAPSULE_V3|HELTEC_VM_T190|"
    "HELTEC_VM_E213|HELTEC_VM_E290|HELTEC_T114|SENSECAP_INDICATOR|TRACKER_T1000_E|RAK3172|WIO_E5|RM_900_BANDIT|"
    "ME25LS01_4Y10TD|RP2040_FEATHER_RFM95|M5STACK_COREBASIC|M5STACK_CORE2|RPI_PICO2|M5STACK_CORES3|SEEED_XIAO_S3|"
    "MS24SF1|TLORA_C6|WISMESH_TAP|ROUTASTIC|MESH_TAB|MESHLINK|XIAO_NRF52_KIT|THINKNODE_M1|THINKNODE_M2|"
    "T_ETH_ELITE|HELTEC_SENSOR_HUB|MUZI_BASE|HELTEC_MESH_POCKET|SEEED_SOLAR_NODE|NOMADSTAR_METEOR_PRO|CROWPANEL|"
    "LINK_32|SEEED_WIO_L1|SEEED_WIO_L1_EINK|MUZI_R1_NEO|T_DECK_PRO|T_LORA_PAGER|M5STACK_RESERVED|WISMESH_TAG|"
    "RAK3312|THINKNODE_M5|HELTEC_MESH_SOLAR|T_ECHO_LITE|HELTEC_V4|M5STACK_C6L|M5_CARDPUTER_ADV|"
    "HELTEC_WL_TRACKER_V2|T_WATCH_ULTRA|THINKNODE_M3|WISMESH_TAP_V2|RAK3401|RAK6421|THINKNODE_M4|THINKNODE_M6|"
    "MESHSTICK_1262|TBEAM_1_WATT|T5_S3_EPAPER_PRO|TBEAM_BPF|MINI_EPAPER_S3|TDISPLAY_S3_PRO|HELTEC_T096|"
    "MESH_TRACKER_X1|THINKNODE_M7|THINKNODE_M8|THINKNODE_M9|HELTEC_V4_R8|HELTEC_MESH_NODE_T1|STATION_G3|"
    "T_IMPULSE_PLUS|T_ECHO_CARD|SEEED_WIO_L2|CROWPANEL_P4|HELTEC_TOWER_V2|MESHNOLOGY_W10|HELTEC_RC32|"
    "HELTEC_RC52|HELTEC_RCC6|SEEED_WIO_L1_PRO_1W|MESHNOLOGY_W12|MESHPAGER_X2|T_CONNECT_PRO|AXIOMETA_GENESIS_MINI|"
    "NOMAD_TERMINAL|THINKNODE_MX|NODARA|HELTEC_MESH_POCKET_V2";

const char *mt_hw_name(uint32_t model)
{
    static char out[24];
    if (model == 255) return "PRIVATE_HW";
    if (model == 254) return "VIRTUAL_NODE";
    const char *p = HW;
    for (uint32_t i = 0; i < model && p; i++) { p = strchr(p, '|'); if (p) p++; }
    if (!p || !*p) return NULL;
    const char *e = strchr(p, '|');
    const size_t n = e ? (size_t)(e - p) : strlen(p);
    snprintf(out, sizeof(out), "%.*s", (int)(n < sizeof(out) - 1 ? n : sizeof(out) - 1), p);
    return out;
}

const char *mt_port_name(uint32_t port)
{
    switch (port) {
    case MT_PORT_TEXT: return "TEXT";
    case MT_PORT_POSITION: return "POSITION";
    case MT_PORT_NODEINFO: return "NODEINFO";
    case MT_PORT_ROUTING: return "ROUTING";
    case MT_PORT_ADMIN: return "ADMIN";
    case MT_PORT_WAYPOINT: return "WAYPOINT";
    case MT_PORT_STORE_FORWARD: return "S&F";
    case MT_PORT_RANGE_TEST: return "RANGE TEST";
    case MT_PORT_TELEMETRY: return "TELEMETRY";
    case MT_PORT_TRACEROUTE: return "TRACEROUTE";
    case MT_PORT_NEIGHBORINFO: return "NEIGHBORS";
    default: return "OTHER";
    }
}
