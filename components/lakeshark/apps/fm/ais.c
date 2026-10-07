#include "ais.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#define TAPS 81
#define RAW_BITS 1024
typedef struct {
    float i[TAPS], q[TAPS], prev_i, prev_q, dc, phase;
    unsigned at, decimate;
    bool sign, sampled, previous, framed;
    uint8_t shift, raw[RAW_BITS], bytes[RAW_BITS/8];
    unsigned count;
} channel_t;
struct ais_ctx {
    channel_t channel[2];
    float fir[TAPS], cosine, sine;
    unsigned oscillator;
    ais_receive_fn receive;
    void *user;
    ais_vessel_t decoded;
};
static uint32_t field(const uint8_t *p, unsigned at, unsigned n)
{
    uint32_t v = 0;
    while (n--) { v = (v<<1) | ((p[at/8] >> (7-at%8)) & 1); ++at; }
    return v;
}
static int32_t signed_field(const uint8_t *p, unsigned at, unsigned n)
{
    uint32_t v = field(p, at, n);
    return (v & (1u<<(n-1))) ? (int32_t)((int64_t)v-(1LL<<n)) : (int32_t)v;
}
static void text_field(const uint8_t *p, unsigned at, unsigned n, char *out)
{
    for (unsigned i = 0; i < n; ++i) {
        unsigned v = field(p, at+6*i, 6);
        out[i] = v < 32 ? v+64 : v;
    }
    while (n && (out[n-1] == '@' || out[n-1] == ' ')) --n;
    out[n] = 0;
}
static void position(const uint8_t *p, unsigned at, ais_vessel_t *o)
{
    o->fields |= AIS_POSITION;
    o->lon = signed_field(p, at, 28)/600000.0;
    o->lat = signed_field(p, at+28, 27)/600000.0;
    o->position = fabs(o->lat) <= 90 && fabs(o->lon) <= 180;
}
bool ais_parse(const uint8_t *p, size_t bits, ais_vessel_t *o)
{
    if (!p || !o || bits < 38) return false;
    unsigned type = field(p, 0, 6), minimum;
    switch (type) {
    case 1: case 2: case 3: case 4: case 18: minimum = 168; break;
    case 5: minimum = 424; break;
    case 19: minimum = 312; break;
    case 24:
        if (bits < 40 || field(p, 38, 2) > 1) return false;
        minimum = field(p, 38, 2) ? 168 : 160; break;
    default: return false;
    }
    if (bits < minimum || bits > minimum+7) return false;
    memset(o, 0, sizeof(*o));
    o->message = type; o->mmsi = field(p, 8, 30);
    if (!o->mmsi || o->mmsi > 999999999) return false;
    o->sog = 1023; o->cog = 3600; o->heading = 511; o->nav_status = 15;
    if (type <= 3) {
        o->fields |= AIS_MOTION; o->nav_status = field(p, 38, 4);
        o->sog = field(p, 50, 10); position(p, 61, o);
        o->cog = field(p, 116, 12); o->heading = field(p, 128, 9);
    } else if (type == 4) position(p, 79, o);
    else if (type == 18 || type == 19) {
        o->fields |= AIS_MOTION; o->sog = field(p, 46, 10); position(p, 57, o);
        o->cog = field(p, 112, 12); o->heading = field(p, 124, 9);
        if (type == 19) {
            o->fields |= AIS_NAME | AIS_STATIC;
            text_field(p, 143, 20, o->name); o->ship_type = field(p, 263, 8);
        }
    } else if (type == 5) {
        o->fields = AIS_NAME | AIS_STATIC | AIS_VOYAGE;
        text_field(p, 70, 7, o->callsign); text_field(p, 112, 20, o->name);
        o->ship_type = field(p, 232, 8); text_field(p, 302, 20, o->destination);
    } else if (!field(p, 38, 2)) {
        o->fields = AIS_NAME; text_field(p, 40, 20, o->name);
    } else {
        o->fields = AIS_STATIC; o->ship_type = field(p, 40, 8);
        text_field(p, 90, 7, o->callsign);
    }
    return true;
}
uint16_t ais_fcs(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xffff;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; ++i) crc = (crc>>1) ^ ((crc&1) ? 0x8408 : 0);
    }
    return crc;
}
static void frame(ais_ctx_t *ctx, unsigned channel, unsigned n)
{
    channel_t *c = &ctx->channel[channel];
    unsigned ones = 0, bits = 0;
    memset(c->bytes, 0, sizeof(c->bytes));
    for (unsigned i = 0; i < n; ++i) {
        unsigned b = c->raw[i];
        if (ones == 5) { if (b) return; ones = 0; continue; }
        if (b) ++ones; else ones = 0;
        c->bytes[bits/8] |= b<<(bits%8); ++bits;
    }
    if (bits < 56 || bits%8 || ais_fcs(c->bytes, bits/8) != 0xf0b8) return;
    if (ais_parse(c->bytes, bits-16, &ctx->decoded)) {
        ctx->decoded.channel = channel;
        if (ctx->receive) ctx->receive(&ctx->decoded, ctx->user);
    }
}
void ais_symbol(ais_ctx_t *ctx, unsigned channel, bool level)
{
    if (!ctx || channel > 1) return;
    channel_t *c = &ctx->channel[channel];
    unsigned b = level == c->previous; c->previous = level;
    c->shift = (c->shift>>1) | (b<<7);
    if (c->framed) {
        if (c->count == RAW_BITS) { c->framed = false; c->count = 0; }
        else c->raw[c->count++] = b;
    }
    if (c->shift == 0x7e) {
        if (c->framed && c->count >= 8) frame(ctx, channel, c->count-8);
        c->framed = true; c->count = 0;
    }
}
void ais_reset(ais_ctx_t *ctx)
{
    if (!ctx) return;
    memset(ctx->channel, 0, sizeof(ctx->channel));
    ctx->cosine = 1; ctx->sine = 0; ctx->oscillator = 0;
}
ais_ctx_t *ais_create(ais_receive_fn receive, void *user)
{
#ifdef ESP_PLATFORM
    ais_ctx_t *ctx = heap_caps_calloc(1, sizeof(*ctx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    ais_ctx_t *ctx = calloc(1, sizeof(*ctx));
#endif
    if (!ctx) return NULL;
    ctx->receive = receive; ctx->user = user;
    float sum = 0;
    for (int i = 0; i < TAPS; ++i) {
        int x = i-TAPS/2;
        float sinc = x ? sinf(2*M_PI*12000*x/AIS_SAMPLE_RATE)/(M_PI*x) : 24000.0f/AIS_SAMPLE_RATE;
        ctx->fir[i] = sinc*(0.54f-0.46f*cosf(2*M_PI*i/(TAPS-1))); sum += ctx->fir[i];
    }
    for (int i = 0; i < TAPS; ++i) ctx->fir[i] /= sum;
    ais_reset(ctx); return ctx;
}
void ais_destroy(ais_ctx_t *ctx) { free(ctx); }
static void discriminator(ais_ctx_t *ctx, unsigned ch, float i, float q)
{
    channel_t *c = &ctx->channel[ch];
    float d = atan2f(c->prev_i*q-c->prev_q*i, c->prev_i*i+c->prev_q*q);
    c->prev_i = i; c->prev_q = q;
    /* Remove modest carrier error without following the GMSK symbols. */
    c->dc += 0.001f*(d-c->dc); d -= c->dc;
    bool sign = d >= 0;
    if (sign != c->sign) {
        float error = c->phase > 0.5f ? c->phase-1 : c->phase;
        c->phase -= 0.2f*error;
        if (c->phase < 0) c->phase += 1;
    }
    c->sign = sign;
    c->phase += 9600.0f/64000;
    if (!c->sampled && c->phase >= 0.5f) { ais_symbol(ctx, ch, sign); c->sampled = true; }
    if (c->phase >= 1) { c->phase -= 1; c->sampled = false; }
}
void ais_process(ais_ctx_t *ctx, const uint8_t *iq, size_t bytes)
{
    if (!ctx || !iq) return;
    const float step = 2*M_PI*25000/AIS_SAMPLE_RATE;
    const float cs = cosf(step), sn = sinf(step);
    for (size_t j = 0; j+1 < bytes; j += 2) {
        float i = (float)iq[j]-127.5f, q = (float)iq[j+1]-127.5f;
        for (unsigned ch = 0; ch < 2; ++ch) {
            channel_t *c = &ctx->channel[ch];
            float sine = ch ? -ctx->sine : ctx->sine;
            c->i[c->at] = i*ctx->cosine-q*sine;
            c->q[c->at] = i*sine+q*ctx->cosine;
            c->at = (c->at+1)%TAPS;
            if (++c->decimate == 4) {
                c->decimate = 0; float fi = 0, fq = 0;
                unsigned at = c->at;
                for (int k = 0; k < TAPS; ++k) {
                    fi += ctx->fir[k]*c->i[at]; fq += ctx->fir[k]*c->q[at];
                    at = (at+1)%TAPS;
                }
                discriminator(ctx, ch, fi, fq);
            }
        }
        float co = ctx->cosine*cs-ctx->sine*sn;
        ctx->sine = ctx->sine*cs+ctx->cosine*sn; ctx->cosine = co;
        /* Bound oscillator roundoff across an indefinitely running stream. */
        if (++ctx->oscillator == 1024) {
            ctx->oscillator = 0;
            float norm = hypotf(ctx->cosine, ctx->sine);
            ctx->cosine /= norm; ctx->sine /= norm;
        }
    }
}
