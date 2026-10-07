#include "aprs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static void copy_text(char *out, size_t cap, const uint8_t *p, size_t n)
{
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; ++i) out[i] = p[i] >= 32 && p[i] < 127 ? p[i] : '.';
    out[n] = 0;
}
static int digits(const uint8_t *p, int n)
{
    int v = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + p[i] - '0';
    }
    return v;
}
static bool coordinate(aprs_packet_t *o, double lat, double lon)
{
    if (!isfinite(lat) || !isfinite(lon) || fabs(lat) > 90 || fabs(lon) > 180) return false;
    o->lat = lat; o->lon = lon; o->position = true;
    return true;
}
static int minute_digits(int d[4])
{
    int missing = 0;
    for (int i = 0; i < 4; ++i) {
        if (d[i] == -2) ++missing;
        else if (d[i] < 0 || missing) return -1;
    }
    if (missing) {
        for (int i = 4-missing; i < 4; ++i) d[i] = 0;
        /* Plot the centre of the stated precision box. */
        d[4-missing] = missing == 4 ? 3 : 5;
    }
    return missing;
}
static int digit_or_space(uint8_t c)
{ return c == ' ' ? -2 : c >= '0' && c <= '9' ? c-'0' : -1; }
static size_t position(const uint8_t *p, size_t n, aprs_packet_t *o)
{
    if (n >= 19 && (p[7] == 'N' || p[7] == 'S')) {
        int la[] = {digit_or_space(p[2]), digit_or_space(p[3]), digit_or_space(p[5]), digit_or_space(p[6])};
        int lo[] = {digit_or_space(p[12]), digit_or_space(p[13]), digit_or_space(p[15]), digit_or_space(p[16])};
        int ambiguous = minute_digits(la), other = minute_digits(lo);
        if (ambiguous < 0 || other != ambiguous) return 0;
        o->ambiguous = ambiguous != 0;
        int a = digits(p, 2), b = la[0]*10+la[1], c = la[2]*10+la[3];
        int d = digits(p + 9, 3), e = lo[0]*10+lo[1], f = lo[2]*10+lo[3];
        if (a < 0 || b < 0 || c < 0 || d < 0 || e < 0 || f < 0 ||
            b >= 60 || e >= 60 || p[4] != '.' || p[14] != '.' ||
            (p[17] != 'E' && p[17] != 'W') || p[18] < 33 || p[18] > 126) return 0;
        double lat = a + (b + c / 100.0) / 60.0, lon = d + (e + f / 100.0) / 60.0;
        if (!coordinate(o, p[7] == 'S' ? -lat : lat, p[17] == 'W' ? -lon : lon)) return 0;
        o->table = p[8]; o->symbol = p[18]; return 19;
    }
    if (n < 13 || p[9] < 33 || p[9] > 126 ||
        !(p[0] == '/' || p[0] == '\\' || (p[0] >= 'A' && p[0] <= 'Z') ||
          (p[0] >= 'a' && p[0] <= 'j'))) return 0;
    uint32_t lat = 0, lon = 0;
    for (int i = 1; i <= 8; ++i) {
        if (p[i] < 33 || p[i] > 123) return 0;
        if (i <= 4) lat = lat * 91 + p[i] - 33;
        else lon = lon * 91 + p[i] - 33;
    }
    if (!coordinate(o, 90 - lat / 380926.0, -180 + lon / 190463.0)) return 0;
    o->table = p[0]; o->symbol = p[9]; return 13;
}
static void weather(const uint8_t *p, size_t n, aprs_packet_t *o)
{
    o->weather = true;
    if (o->kind == APRS_POSITION) o->kind = APRS_WEATHER;
    bool direction = false, speed_known = false;
    if (n >= 7 && p[3] == '/') {
        int dir = digits(p, 3), speed = digits(p + 4, 3);
        if (dir >= 0 && dir <= 360 && speed >= 0) {
            o->wind_valid = true; o->wind_deg = dir; o->wind_mph = speed;
            direction = speed_known = true;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == 't' && i + 3 < n) {
            int v = p[i+1] == '-' ? digits(p+i+2, 2) : digits(p+i+1, 3);
            if (v >= 0) { o->temperature_valid = true; o->temperature_f = p[i+1] == '-' ? -v : v; }
        } else if (p[i] == 'h' && i + 2 < n) {
            int v = digits(p+i+1, 2);
            if (v >= 0) { o->humidity_valid = true; o->humidity = v ? v : 100; }
        } else if (p[i] == 'c' && i + 3 < n && digits(p+i+1, 3) >= 0 && digits(p+i+1, 3) <= 360) {
            o->wind_deg = digits(p+i+1, 3);
            direction = true;
        } else if (p[i] == 's' && i + 3 < n && digits(p+i+1, 3) >= 0) {
            o->wind_mph = digits(p+i+1, 3); speed_known = true;
        } else if (p[i] == 'g' && i + 3 < n && digits(p+i+1, 3) >= 0) {
            o->gust_mph = digits(p+i+1, 3); o->gust_valid = true;
        } else if (p[i] == 'r' && i + 3 < n && digits(p+i+1, 3) >= 0) {
            o->rain_hour_hundredths = digits(p+i+1, 3); o->rain_valid = true;
        } else if (p[i] == 'b' && i + 5 < n && digits(p+i+1, 5) >= 0) {
            o->pressure_tenths_hpa = digits(p+i+1, 5); o->pressure_valid = true;
        }
    }
    o->wind_valid = direction && speed_known;
}
bool aprs_parse(const char *src, const char *dst, const uint8_t *p, size_t n, aprs_packet_t *o)
{
    if (!src || !dst || !p || !n || !o) return false;
    memset(o, 0, sizeof(*o));
    snprintf(o->call, sizeof(o->call), "%s", src); o->symbol = '?';
    size_t offset = 1;
    switch (p[0]) {
    case ':':
        if (n < 11 || p[10] != ':') return false;
        o->kind = APRS_MESSAGE; copy_text(o->recipient, sizeof(o->recipient), p+1, 9);
        for (int i = 8; i >= 0 && o->recipient[i] == ' '; --i) o->recipient[i] = 0;
        copy_text(o->text, sizeof(o->text), p+11, n-11); return true;
    case '>':
        o->kind = APRS_STATUS; copy_text(o->text, sizeof(o->text), p+1, n-1); return true;
    case '_':
        if (n < 9) return false;
        weather(p+9, n-9, o); copy_text(o->text, sizeof(o->text), p+9, n-9); return true;
    case ';':
        if (n < 18 || (p[10] != '*' && p[10] != '_')) return false;
        o->kind = APRS_OBJECT; o->killed = p[10] == '_';
        copy_text(o->name, sizeof(o->name), p+1, 9);
        for (int i = 8; i >= 0 && o->name[i] == ' '; --i) o->name[i] = 0;
        offset = 18; break;
    case ')':
        offset = 1;
        while (offset < n && offset <= 9 && p[offset] != '!' && p[offset] != '_') ++offset;
        if (offset >= n || offset > 9 || offset == 1) return false;
        o->kind = APRS_ITEM; o->killed = p[offset] == '_';
        copy_text(o->name, sizeof(o->name), p+1, offset-1); ++offset; break;
    case '/': case '@':
        if (n < 8) return false;
        offset = 8; break;
    case '!': case '=': break;
    case '`': case '\'': {
        if (n < 9 || strlen(dst) < 6 || p[7] < 33 || p[7] > 126 ||
            (p[8] != '/' && p[8] != '\\')) return false;
        int d[6];
        for (int i = 0; i < 6; ++i) {
            int c = (unsigned char)dst[i];
            d[i] = c >= '0' && c <= '9' ? c-'0' : c >= 'A' && c <= 'J' ? c-'A' :
                   c >= 'P' && c <= 'Y' ? c-'P' : c == 'K' || c == 'L' || c == 'Z' ? -2 : -1;
            if (d[i] == -1 || (i < 2 && d[i] < 0)) return false;
        }
        int ambiguous = minute_digits(d+2);
        if (ambiguous < 0) return false;
        o->ambiguous = ambiguous != 0;
        int minutes = d[2]*10+d[3];
        int deg = (int)p[1]-28, min = (int)p[2]-28, hundredths = (int)p[3]-28;
        if (dst[4] >= 'P') deg += 100;
        if (deg >= 180 && deg <= 189) deg -= 80;
        else if (deg >= 190 && deg <= 199) deg -= 190;
        if (min >= 60) min -= 60;
        if (minutes >= 60 || deg < 0 || min < 0 || min >= 60 || hundredths < 0 || hundredths > 99) return false;
        if (ambiguous == 1) hundredths = hundredths/10*10+5;
        else if (ambiguous == 2) hundredths = 50;
        else if (ambiguous == 3) { min = min/10*10+5; hundredths = 0; }
        else if (ambiguous == 4) { min = 30; hundredths = 0; }
        double lat = d[0]*10+d[1]+(minutes+(d[4]*10+d[5])/100.0)/60;
        double lon = deg+(min+hundredths/100.0)/60;
        if (!coordinate(o, dst[3] >= 'P' ? lat : -lat, dst[5] >= 'P' ? -lon : lon)) return false;
        o->symbol = p[7]; o->table = p[8];
        copy_text(o->text, sizeof(o->text), p+9, n-9); return true;
    }
    default: return false;
    }
    size_t used = position(p+offset, n-offset, o);
    if (!used) return false;
    offset += used;
    copy_text(o->text, sizeof(o->text), p+offset, n-offset);
    if (o->symbol == '_') weather(p+offset, n-offset, o);
    return true;
}

/* Phase lanes avoid depending on a particular audio block or symbol boundary.
   Only a complete CRC-valid UI frame can publish a packet. */
#define LANES 16
#define RAW_BYTES 620
typedef struct {
    float mi, mq, si, sq;
    unsigned phase, previous, shift, bits;
    bool active;
    uint8_t raw[RAW_BYTES];
} lane_t;
struct aprs_ctx {
    lane_t lane[LANES];
    float tone[80][4];
    unsigned sample_phase;
    uint64_t samples, last_sample;
    uint32_t last_hash;
    float dc;
    bool last_valid;
    uint8_t frame[APRS_FRAME_MAX];
    aprs_packet_t packet;
    aprs_callback_t callback;
    void *user;
};
uint16_t aprs_crc(const uint8_t *p, size_t n)
{
    /* AX.25 uses reflected CCITT with an initial FFFF; a frame's residue is F0B8. */
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
    }
    return crc;
}
static bool address(const uint8_t *p, char *out, size_t cap)
{
    int used = 0;
    for (int i = 0; i < 6; ++i) {
        if (p[i] & 1) return false;
        unsigned c = p[i] >> 1;
        if (c < 32 || c > 126) return false;
        if (c != ' ') out[used++] = c;
    }
    if (!used) return false;
    unsigned ssid = (p[6] >> 1) & 15;
    if (ssid) used += snprintf(out+used, cap-used, "-%u", ssid);
    out[used] = 0; return true;
}
bool aprs_frame(aprs_ctx_t *s, const uint8_t *p, size_t n)
{
    if (!s || !p || n < 19 || n > APRS_FRAME_MAX || aprs_crc(p, n) != 0xf0b8) return false;
    char src[12], dst[12];
    if ((p[6] & 1) || !address(p, dst, sizeof(dst)) || !address(p+7, src, sizeof(src))) return false;
    size_t offset = 14; unsigned addresses = 2;
    while (!(p[offset-1] & 1)) {
        char path[12];
        if (++addresses > 10 || offset+7 > n-4 || !address(p+offset, path, sizeof(path))) return false;
        offset += 7;
    }
    if (offset+2 >= n-2 || (p[offset] & 0xef) != 3 || p[offset+1] != 0xf0) return false;
    if (!aprs_parse(src, dst, p+offset+2, n-offset-4, &s->packet)) return false;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < n; ++i) hash = (hash ^ p[i]) * 16777619u;
    /* Adjacent phase lanes see the same frame a few samples apart. */
    if (!s->last_valid || hash != s->last_hash || s->samples-s->last_sample > APRS_SAMPLE_RATE/2) {
        s->last_hash = hash; s->last_sample = s->samples; s->last_valid = true;
        if (s->callback) s->callback(&s->packet, s->user);
    }
    return true;
}
static void bit(aprs_ctx_t *s, lane_t *l, unsigned value)
{
    value &= 1;
    l->shift = ((l->shift >> 1) | (value << 7)) & 255;
    if (l->active) {
        if (l->bits >= RAW_BYTES*8) { l->active = false; l->bits = 0; }
        else {
            unsigned b = l->bits++;
            if (!(b & 7)) l->raw[b/8] = 0;
            l->raw[b/8] |= value << (b & 7);
        }
    }
    if (l->shift != 0x7e) return;
    if (l->active && l->bits > 8) {
        unsigned ones = 0, count = 0; bool valid = true;
        memset(s->frame, 0, sizeof(s->frame));
        for (unsigned i = 0; i < l->bits-8; ++i) {
            unsigned v = (l->raw[i/8] >> (i & 7)) & 1;
            if (ones == 5) {
                if (v) { valid = false; break; }
                ones = 0; continue;
            }
            ones = v ? ones+1 : 0;
            if (count >= APRS_FRAME_MAX*8) { valid = false; break; }
            s->frame[count/8] |= v << (count & 7); ++count;
        }
        if (valid && !(count & 7)) aprs_frame(s, s->frame, count/8);
    }
    l->active = true; l->bits = 0;
}
void aprs_bit(aprs_ctx_t *s, unsigned b) { if (s) bit(s, &s->lane[0], b); }
void aprs_reset(aprs_ctx_t *s)
{
    if (!s) return;
    memset(s->lane, 0, sizeof(s->lane));
    for (int i = 0; i < LANES; ++i) s->lane[i].phase = i*APRS_SAMPLE_RATE/LANES;
    s->sample_phase = 0; s->samples = 0; s->last_valid = false;
    s->dc = 0;
}
aprs_ctx_t *aprs_create(aprs_callback_t cb, void *user)
{
#ifdef ESP_PLATFORM
    aprs_ctx_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    aprs_ctx_t *s = calloc(1, sizeof(*s));
#endif
    if (!s) return NULL;
    s->callback = cb; s->user = user;
    for (int i = 0; i < 80; ++i) {
        double m = 6.283185307179586*1200*i/APRS_SAMPLE_RATE;
        double q = 6.283185307179586*2200*i/APRS_SAMPLE_RATE;
        s->tone[i][0] = cos(m); s->tone[i][1] = sin(m);
        s->tone[i][2] = cos(q); s->tone[i][3] = sin(q);
    }
    aprs_reset(s); return s;
}
void aprs_destroy(aprs_ctx_t *s) { free(s); }
void aprs_process(aprs_ctx_t *s, const int16_t *pcm, int n)
{
    if (!s || !pcm || n <= 0) return;
    for (int k = 0; k < n; ++k) {
        float x = pcm[k]/32768.0f;
        /* Discriminator offset must not look like energy in the space tone. */
        s->dc += 0.01f*(x-s->dc); x -= s->dc;
        const float *t = s->tone[s->sample_phase];
        for (int j = 0; j < LANES; ++j) {
            lane_t *l = &s->lane[j];
            l->mi += x*t[0]; l->mq += x*t[1]; l->si += x*t[2]; l->sq += x*t[3];
            l->phase += 1200;
            if (l->phase >= APRS_SAMPLE_RATE) {
                l->phase -= APRS_SAMPLE_RATE;
                unsigned tone = l->mi*l->mi+l->mq*l->mq >= l->si*l->si+l->sq*l->sq;
                bit(s, l, tone == l->previous); l->previous = tone;
                l->mi = l->mq = l->si = l->sq = 0;
            }
        }
        s->sample_phase = (s->sample_phase+1)%80; ++s->samples;
    }
}
