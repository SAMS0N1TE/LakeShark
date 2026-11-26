/* PARTY's wire format (party_proto.h). Pure C, no allocation: the bench
   tests it as it is. */
#include "party_proto.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "../wardrive/wd_core.h"

/* ------------------------------------------------------------ classes */

static const char *const CNAME[PARTY_CLASSES] = {
    "MARK", "ALPR", "CAMERA", "BODYCAM", "POLICE", "DRONE", "TRACKER", "VEHICLE",
    "FOLLOWING", "SKIMMER", "HACK TOOL", "WEARABLE", "INFRA", "WATCHED", "KEYWORD", "HELP",
};
static const char *const CTAG[PARTY_CLASSES] = {
    "MRK", "LPR", "CAM", "BWC", "POL", "UAS", "TAG", "CAR",
    "FOL", "SKM", "HAK", "WRB", "INF", "WCH", "KEY", "SOS",
};
const char *party_class_name(int c) { return c >= 0 && c < PARTY_CLASSES ? CNAME[c] : "?"; }
const char *party_class_tag(int c) { return c >= 0 && c < PARTY_CLASSES ? CTAG[c] : "?"; }

/* Most telling first: a plate reader is also a camera, a police car also a vehicle. */
static const struct { uint16_t tag; uint8_t cls; } PRIO[] = {
    { WD_T_ALPR, PARTY_C_ALPR },       { WD_T_BODYCAM, PARTY_C_BODYCAM }, { WD_T_POLICE, PARTY_C_POLICE },
    { WD_T_DRONE, PARTY_C_DRONE },     { WD_T_FOLLOW, PARTY_C_FOLLOW },   { WD_T_TRACKER, PARTY_C_TRACKER },
    { WD_T_SKIMMER, PARTY_C_SKIMMER }, { WD_T_HACKTOOL, PARTY_C_HACKTOOL }, { WD_T_WEARABLE, PARTY_C_WEARABLE },
    { WD_T_WATCH, PARTY_C_WATCH },     { WD_T_CAMERA, PARTY_C_CAMERA },   { WD_T_VEHICLE, PARTY_C_VEHICLE },
    { WD_T_INFRA, PARTY_C_INFRA },
};

static bool has_word(const char *name, const char *kw)
{
    const size_t k = strlen(kw);
    if (!k) return false;
    for (const char *p = name; *p; p++) {
        size_t i = 0;
        while (i < k && p[i] && toupper((unsigned char)p[i]) == toupper((unsigned char)kw[i])) i++;
        if (i == k) return true;
    }
    return false;
}

int party_class_of(uint16_t wd_tags, const char *name, const char (*kw)[13], int nkw)
{
    for (unsigned i = 0; i < sizeof(PRIO) / sizeof(PRIO[0]); i++)
        if (wd_tags & PRIO[i].tag) return PRIO[i].cls;
    if (name && kw)
        for (int i = 0; i < nkw; i++)
            if (kw[i][0] && has_word(name, kw[i])) return PARTY_C_CUSTOM;
    return -1;
}

/* ------------------------------------------------------------ bytes */

typedef struct { uint8_t *p; int n, cap; bool bad; } wr_t;
static void w8(wr_t *w, unsigned v) { if (w->n < w->cap) w->p[w->n++] = (uint8_t)v; else w->bad = true; }
static void w16(wr_t *w, unsigned v) { w8(w, v); w8(w, v >> 8); }
static void w32(wr_t *w, uint32_t v) { w16(w, v & 0xFFFF); w16(w, v >> 16); }
static void wbytes(wr_t *w, const void *s, int n) { for (int i = 0; i < n; i++) w8(w, ((const uint8_t *)s)[i]); }
static void wstr(wr_t *w, const char *s, int max)
{
    int n = (int)strnlen(s, (size_t)max);
    w8(w, (unsigned)n);
    wbytes(w, s, n);
}

typedef struct { const uint8_t *p; int n, at; bool bad; } rd_t;
static unsigned r8(rd_t *r) { if (r->at < r->n) return r->p[r->at++]; r->bad = true; return 0; }
static unsigned r16(rd_t *r) { unsigned v = r8(r); return v | (r8(r) << 8); }
static uint32_t r32(rd_t *r) { uint32_t v = r16(r); return v | ((uint32_t)r16(r) << 16); }
static void rbytes(rd_t *r, void *d, int n) { for (int i = 0; i < n; i++) ((uint8_t *)d)[i] = (uint8_t)r8(r); }
static void rstr(rd_t *r, char *d, int max)
{
    int n = (int)r8(r);
    if (n > max) { r->bad = true; n = 0; }
    rbytes(r, d, n);
    d[n] = 0;
    for (int i = 0; i < n; i++) if ((unsigned char)d[i] < 32 || (unsigned char)d[i] > 126) d[i] = '?';
}

static wr_t head(uint8_t *out, int cap, party_hdr_t *h, int type)
{
    wr_t w = { out, 0, cap < PARTY_DGRAM_MAX ? cap : PARTY_DGRAM_MAX, false };
    h->type = (uint8_t)type;
    w8(&w, (PARTY_PROTO_VER << 5) | (type & 0x1F));
    w16(&w, h->party);
    w8(&w, h->src); w8(&w, h->dst); w8(&w, h->seq);
    return w;
}
static int done(const wr_t *w) { return w->bad ? -1 : w->n; }

int party_enc_hello(uint8_t *out, int cap, party_hdr_t *h, const party_hello_t *m)
{
    wr_t w = head(out, cap, h, PARTY_HELLO);
    char call[PARTY_CALL_MAX];
    memset(call, 0, sizeof(call));
    memcpy(call, m->call, strnlen(m->call, PARTY_CALL_MAX));
    wbytes(&w, call, PARTY_CALL_MAX);
    w8(&w, m->glyph); w8(&w, m->colour); w8(&w, m->caps); w8(&w, m->share);
    w16(&w, m->want); w8(&w, m->batt); w8(&w, m->flags);
    return done(&w);
}

int party_enc_pos(uint8_t *out, int cap, party_hdr_t *h, const party_pos_t *m)
{
    wr_t w = head(out, cap, h, PARTY_POS);
    w32(&w, (uint32_t)m->lat_e7); w32(&w, (uint32_t)m->lon_e7);
    w16(&w, m->acc_m); w8(&w, m->speed_half); w8(&w, m->course); w8(&w, m->heading);
    w8(&w, m->batt); w8(&w, m->flags);
    return done(&w);
}

int party_enc_ping(uint8_t *out, int cap, party_hdr_t *h, const party_ping_t *m)
{
    wr_t w = head(out, cap, h, PARTY_PING);
    w16(&w, m->id); w8(&w, m->cls); w8(&w, m->kind); w16(&w, m->tags);
    w8(&w, (uint8_t)m->rssi); w8(&w, m->flags);
    w32(&w, (uint32_t)m->lat_e7); w32(&w, (uint32_t)m->lon_e7);
    w16(&w, m->unc_m); w16(&w, m->age_s);
    wbytes(&w, m->addr, 6);
    wstr(&w, m->label, PARTY_LABEL_MAX);
    return done(&w);
}

int party_enc_offer(uint8_t *out, int cap, party_hdr_t *h, const party_offer_t *m)
{
    wr_t w = head(out, cap, h, PARTY_OFFER);
    w16(&w, m->fid); w32(&w, m->size); w32(&w, m->crc); w8(&w, m->chunk);
    wstr(&w, m->name, PARTY_FNAME_MAX);
    return done(&w);
}

int party_enc_chunk(uint8_t *out, int cap, party_hdr_t *h, const party_chunk_t *m)
{
    wr_t w = head(out, cap, h, PARTY_CHUNK);
    w16(&w, m->fid); w16(&w, m->idx); w8(&w, m->len);
    wbytes(&w, m->data, m->len);
    return done(&w);
}

int party_enc_ack(uint8_t *out, int cap, party_hdr_t *h, const party_ack_t *m)
{
    wr_t w = head(out, cap, h, PARTY_ACK);
    w16(&w, m->fid); w16(&w, m->base); w32(&w, m->bits); w8(&w, m->flags);
    return done(&w);
}

int party_enc_bye(uint8_t *out, int cap, party_hdr_t *h)
{
    wr_t w = head(out, cap, h, PARTY_BYE);
    return done(&w);
}

int party_decode(const uint8_t *d, int n, uint16_t party, party_hdr_t *h, party_msg_t *m)
{
    if (n < PARTY_HDR) return PARTY_E_SHORT;
    if ((d[0] >> 5) != PARTY_PROTO_VER) return PARTY_E_VERSION;
    rd_t r = { d, n, 1, false };
    h->type = d[0] & 0x1F;
    h->party = (uint16_t)r16(&r);
    h->src = (uint8_t)r8(&r); h->dst = (uint8_t)r8(&r); h->seq = (uint8_t)r8(&r);
    if (h->party != party) return PARTY_E_PARTY;
    memset(m, 0, sizeof(*m));
    switch (h->type) {
    case PARTY_HELLO:
        rbytes(&r, m->hello.call, PARTY_CALL_MAX);
        m->hello.call[PARTY_CALL_MAX] = 0;
        for (int i = 0; m->hello.call[i]; i++)
            if ((unsigned char)m->hello.call[i] < 32 || (unsigned char)m->hello.call[i] > 126) m->hello.call[i] = '?';
        m->hello.glyph = (uint8_t)r8(&r); m->hello.colour = (uint8_t)r8(&r);
        m->hello.caps = (uint8_t)r8(&r); m->hello.share = (uint8_t)r8(&r);
        m->hello.want = (uint16_t)r16(&r); m->hello.batt = (uint8_t)r8(&r); m->hello.flags = (uint8_t)r8(&r);
        break;
    case PARTY_POS:
        m->pos.lat_e7 = (int32_t)r32(&r); m->pos.lon_e7 = (int32_t)r32(&r);
        m->pos.acc_m = (uint16_t)r16(&r); m->pos.speed_half = (uint8_t)r8(&r);
        m->pos.course = (uint8_t)r8(&r); m->pos.heading = (uint8_t)r8(&r);
        m->pos.batt = (uint8_t)r8(&r); m->pos.flags = (uint8_t)r8(&r);
        break;
    case PARTY_PING:
        m->ping.id = (uint16_t)r16(&r); m->ping.cls = (uint8_t)r8(&r); m->ping.kind = (uint8_t)r8(&r);
        m->ping.tags = (uint16_t)r16(&r); m->ping.rssi = (int8_t)r8(&r); m->ping.flags = (uint8_t)r8(&r);
        m->ping.lat_e7 = (int32_t)r32(&r); m->ping.lon_e7 = (int32_t)r32(&r);
        m->ping.unc_m = (uint16_t)r16(&r); m->ping.age_s = (uint16_t)r16(&r);
        rbytes(&r, m->ping.addr, 6);
        rstr(&r, m->ping.label, PARTY_LABEL_MAX);
        if (m->ping.cls >= PARTY_CLASSES) r.bad = true;
        break;
    case PARTY_OFFER:
        m->offer.fid = (uint16_t)r16(&r); m->offer.size = r32(&r); m->offer.crc = r32(&r);
        m->offer.chunk = (uint8_t)r8(&r);
        rstr(&r, m->offer.name, PARTY_FNAME_MAX);
        break;
    case PARTY_CHUNK:
        m->chunk.fid = (uint16_t)r16(&r); m->chunk.idx = (uint16_t)r16(&r); m->chunk.len = (uint8_t)r8(&r);
        if (r.at + m->chunk.len > n) r.bad = true;
        else { m->chunk.data = d + r.at; r.at += m->chunk.len; }
        break;
    case PARTY_ACK:
        m->ack.fid = (uint16_t)r16(&r); m->ack.base = (uint16_t)r16(&r);
        m->ack.bits = r32(&r); m->ack.flags = (uint8_t)r8(&r);
        break;
    case PARTY_BYE:
        break;
    default:
        return PARTY_E_TYPE;
    }
    return r.bad ? PARTY_E_BODY : h->type;
}

int32_t party_deg_e7(double deg) { return (int32_t)lround(deg * 1e7); }
double  party_e7_deg(int32_t e7) { return e7 / 1e7; }
double  party_rough(double deg, double step) { return (floor(deg / step) + 0.5) * step; }

/* ------------------------------------------------------------ the code */

void party_code_norm(const char *code, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = code ? code : ""; *p && n + 1 < cap; p++)
        if (isalnum((unsigned char)*p)) out[n++] = (char)toupper((unsigned char)*p);
    if (cap) out[n] = 0;
}

bool party_key_derive(const char *code, party_key_t *out)
{
    char norm[64];
    party_code_norm(code, norm, sizeof(norm));
    memset(out, 0, sizeof(*out));
    if (strlen(norm) < 4) return false;
    uint8_t buf[32 + 64], d[32];
    static const char SALT[] = "LakeShark PARTY 1";
    size_t n = strlen(norm);
    memcpy(buf, SALT, sizeof(SALT) - 1);
    memcpy(buf + sizeof(SALT) - 1, norm, n);
    party_sha256(buf, sizeof(SALT) - 1 + n, d);
    for (int i = 1; i < 2048; i++) {
        memcpy(buf, d, 32);
        memcpy(buf + 32, norm, n);
        party_sha256(buf, 32 + n, d);
    }
    memcpy(out->key, d, 16);
    out->party = (uint16_t)(d[16] | (d[17] << 8));
    out->channel = (uint8_t)(d[18] % PARTY_CHANNELS);
    return true;
}

/* ------------------------------------------------------------ files */

static bool bit(const uint8_t *b, int i) { return (b[i >> 3] >> (i & 7)) & 1; }
static void bit_set(uint8_t *b, int i) { b[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static void bit_clr(uint8_t *b, int i) { b[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

uint8_t party_chunk_size(int mtu)
{
    int c = mtu - PARTY_HDR - 5;
    if (c > 180) c = 180;
    if (c < 64) c = 64;
    return (uint8_t)c;
}

bool party_frx_begin(party_frx_t *r, const party_offer_t *o)
{
    memset(r, 0, sizeof(*r));
    if (!o->size || o->size > PARTY_FILE_MAX || o->chunk < 64) return false;
    r->fid = o->fid; r->size = o->size; r->crc = o->crc; r->chunk = o->chunk;
    r->nchunks = (uint16_t)((o->size + o->chunk - 1) / o->chunk);
    return r->nchunks <= PARTY_FILE_CHUNKS;
}

int party_frx_put(party_frx_t *r, const party_chunk_t *c, uint32_t *offset)
{
    if (c->fid != r->fid || c->idx >= r->nchunks) return -1;
    const uint32_t off = (uint32_t)c->idx * r->chunk;
    const uint32_t want = r->size - off < r->chunk ? r->size - off : r->chunk;
    if (c->len != want) return -1;
    if (bit(r->bits, c->idx)) return 0;
    bit_set(r->bits, c->idx);
    r->got++;
    if (offset) *offset = off;
    return 1;
}

bool party_frx_complete(const party_frx_t *r) { return r->nchunks && r->got == r->nchunks; }

void party_frx_ack(const party_frx_t *r, party_ack_t *a)
{
    memset(a, 0, sizeof(*a));
    a->fid = r->fid;
    int base = 0;
    while (base < r->nchunks && bit(r->bits, base)) base++;
    a->base = (uint16_t)base;
    for (int i = 0; i < 32; i++) {
        const int k = base + i;
        if (k >= r->nchunks || bit(r->bits, k)) a->bits |= 1u << i;
    }
    if (party_frx_complete(r)) a->flags |= PARTY_ACK_DONE;
}

void party_ftx_begin(party_ftx_t *t, uint16_t fid, uint32_t size, uint8_t chunk)
{
    memset(t, 0, sizeof(*t));
    t->fid = fid; t->size = size; t->chunk = chunk;
    t->nchunks = (uint16_t)(chunk ? (size + chunk - 1) / chunk : 0);
    if (t->nchunks > PARTY_FILE_CHUNKS) t->nchunks = PARTY_FILE_CHUNKS;
    for (int i = 0; i < t->nchunks; i++) bit_set(t->need, i);
}

int party_ftx_next(party_ftx_t *t)
{
    for (int k = 0; k < t->nchunks; k++) {
        const int i = (t->cursor + k) % t->nchunks;
        if (bit(t->need, i)) {
            bit_clr(t->need, i);
            t->cursor = (uint16_t)((i + 1) % t->nchunks);
            return i;
        }
    }
    return -1;
}

void party_ftx_ack(party_ftx_t *t, const party_ack_t *a)
{
    if (a->fid != t->fid || (a->flags & (PARTY_ACK_DONE | PARTY_ACK_REFUSED))) return;
    for (int i = 0; i < 32; i++) {
        const int k = a->base + i;
        if (k >= t->nchunks) break;
        if (!(a->bits & (1u << i))) bit_set(t->need, k);
    }
}

int party_ftx_pending(const party_ftx_t *t)
{
    int n = 0;
    for (int i = 0; i < t->nchunks; i++) n += bit(t->need, i);
    return n;
}

/* ------------------------------------------------------------ hashes */

uint32_t party_crc32(uint32_t crc, const void *p, size_t n)
{
    const uint8_t *b = p;
    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t *b)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) | ((uint32_t)b[i * 4 + 2] << 8) | b[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        const uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void party_sha256(const void *p, size_t n, uint8_t out[32])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *b = p;
    size_t left = n;
    while (left >= 64) { sha_block(h, b); b += 64; left -= 64; }
    uint8_t tail[128];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, b, left);
    tail[left] = 0x80;
    const size_t tl = left + 9 <= 64 ? 64 : 128;
    const uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha_block(h, tail);
    if (tl == 128) sha_block(h, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24); out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8); out[i * 4 + 3] = (uint8_t)h[i];
    }
}
