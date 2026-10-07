#include "same.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

typedef struct { const char *code, *name; } event_t;
static const event_t events[] = {
    {"TOR", "Tornado warning"}, {"SVR", "Severe thunderstorm warning"},
    {"FFW", "Flash flood warning"}, {"FLW", "Flood warning"},
    {"WSW", "Winter storm warning"}, {"BZW", "Blizzard warning"},
    {"HUW", "Hurricane warning"}, {"TSW", "Tsunami warning"},
    {"EVI", "Evacuation immediate"}, {"CEM", "Civil emergency message"},
    {"RWT", "Required weekly test"}, {"RMT", "Required monthly test"},
    {"EAN", "Emergency action notification"}, {"SPS", "Special weather statement"},
    {"TOA", "Tornado watch"}, {"SVA", "Severe thunderstorm watch"},
    {"FFA", "Flash flood watch"}, {"FLA", "Flood watch"},
    {"HUA", "Hurricane watch"}, {"TSA", "Tsunami watch"},
    {"HWW", "High wind warning"}, {"DSW", "Dust storm warning"},
    {"FRW", "Fire warning"}, {"RHW", "Radiological hazard warning"},
    {"SPW", "Shelter in place warning"}, {"SMW", "Special marine warning"},
    {"ADR", "Administrative message"}, {"NPT", "National periodic test"},
    {"EAT", "Emergency action termination"}, {"LEW", "Law enforcement warning"},
    {"LAE", "Local area emergency"}, {"CAE", "Child abduction emergency"},
    {"CDW", "Civil danger warning"}, {"EQW", "Earthquake warning"},
    {"NUW", "Nuclear power plant warning"}, {"VOW", "Volcano warning"}
};
const char *same_event_name(const char *code)
{
    for (unsigned i = 0; i < sizeof(events)/sizeof(events[0]); ++i)
        if (!strcmp(code, events[i].code)) return events[i].name;
    return "Weather / civil alert";
}
static bool digits(const char *p, int n, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        *v = *v * 10 + (unsigned)(p[i] - '0');
    }
    return true;
}
bool same_parse(const char *h, same_alert_t *a)
{
    if (!h || !a) return false;
    size_t len = strlen(h);
    if (len < 42 || len >= SAME_HEADER_MAX || strncmp(h, "ZCZC-", 5)) return false;
    memset(a, 0, sizeof(*a));
    const char *p = h + 5;
    for (int field = 0; field < 2; ++field) {
        for (int j = 0; j < 3; ++j) if (p[j] < 'A' || p[j] > 'Z') return false;
        if (p[3] != '-') return false;
        memcpy(field ? a->event : a->originator, p, 3); p += 4;
    }
    if (strcmp(a->originator, "WXR") && strcmp(a->originator, "CIV") &&
        strcmp(a->originator, "EAS") && strcmp(a->originator, "PEP")) return false;
    unsigned v;
    do {
        if ((size_t)(h + len - p) < 7 || a->location_count == SAME_LOCATIONS ||
            !digits(p, 6, &v) || v > 999999) return false;
        /* 000000 is national; SS000 addresses every county in a state. */
        if (v && (v / 1000) % 100 == 0) return false;
        a->locations[a->location_count++] = v; p += 6;
        if (*p == '+') break;
        if (*p++ != '-') return false;
    } while (true);
    ++p;
    if ((size_t)(h + len - p) != 22 || !digits(p, 4, &v) || p[4] != '-') return false;
    unsigned hours = v / 100, minutes = v % 100;
    if (minutes > 59 || hours > 99 || (!hours && !minutes)) return false;
    a->valid_minutes = (uint16_t)(hours * 60 + minutes); p += 5;
    if (!digits(p, 3, &v) || !v || v > 366) return false;
    a->day = (uint16_t)v;
    if (!digits(p + 3, 2, &v) || v > 23) return false;
    a->hour = (uint8_t)v;
    if (!digits(p + 5, 2, &v) || v > 59 || p[7] != '-') return false;
    a->minute = (uint8_t)v; p += 8;
    for (int i = 0; i < 8; ++i)
        if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9') ||
              p[i] == ' ' || p[i] == '/')) return false;
    if (p[8] != '-' || p[9]) return false;
    memcpy(a->sender, p, 8); memcpy(a->header, h, len + 1);
    a->test = !strcmp(a->event, "RWT") || !strcmp(a->event, "RMT") || !strcmp(a->event, "NPT");
    return true;
}
bool same_matches(const same_alert_t *a, const same_options_t *o)
{
    if (a->test && !o->include_tests) return false;
    if (!o->only_mine) return true;
    for (int i = 0; i < a->location_count; ++i) {
        unsigned f = a->locations[i] % 100000;
        if (!f) return true;
        for (int j = 0; j < 6; ++j)
            if (o->fips[j] && (f == o->fips[j] ||
                (f % 1000 == 0 && f / 1000 == o->fips[j] / 1000))) return true;
    }
    return false;
}

typedef struct {
    float phase, mi, mq, si, sq;
    float confidence;
    unsigned symbols;
    uint8_t shift, bits, preamble;
    int len;
    char text[SAME_HEADER_MAX];
} lane_t;
struct same_ctx {
    lane_t lanes[8];
    float mc, ms, sc, ss;
    int quiet, locked, repeats;
    uint64_t samples, last_burst;
    uint64_t select_at;
    int candidate;
    float best_confidence;
    char burst[3][SAME_HEADER_MAX];
    same_alert_t scratch, active;
    bool have_active, published;
    same_callback_t callback;
    void *user;
};
void same_reset(same_ctx_t *s)
{
    same_callback_t cb = s->callback; void *u = s->user;
    memset(s, 0, sizeof(*s)); s->callback = cb; s->user = u;
    s->locked = s->candidate = -1; s->mc = s->sc = 1;
    for (int i = 0; i < 8; ++i) s->lanes[i].phase = i / 8.0f;
}
same_ctx_t *same_create(same_callback_t cb, void *u)
{
#ifdef ESP_PLATFORM
    same_ctx_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    same_ctx_t *s = calloc(1, sizeof(*s));
#endif
    if (s) { s->callback = cb; s->user = u; same_reset(s); }
    return s;
}
void same_destroy(same_ctx_t *s) { free(s); }
void same_burst(same_ctx_t *s, const char *h)
{
    if (!s || !h || strlen(h) >= SAME_HEADER_MAX) return;
    if (!strcmp(h, "NNNN")) {
        if (s->have_active && !s->active.ended) {
            s->active.ended = true;
            if (s->callback) s->callback(&s->active, s->user);
        }
        s->repeats = 0; s->published = false; return;
    }
    if (strncmp(h, "ZCZC-", 5)) return;
    if (s->samples - s->last_burst > SAME_SAMPLE_RATE * 15 || s->repeats == 3) {
        s->repeats = 0; s->published = false;
    }
    s->last_burst = s->samples;
    strcpy(s->burst[s->repeats++], h);
    const char *candidate = NULL;
    for (int i = 0; i < s->repeats; ++i)
        for (int j = i + 1; j < s->repeats; ++j)
            if (!strcmp(s->burst[i], s->burst[j])) candidate = s->burst[i];
    if (!candidate && s->repeats == 3) {
        size_t n = strlen(s->burst[0]);
        if (n != strlen(s->burst[1]) || n != strlen(s->burst[2])) return;
        /* Vote bits, since a tone error can damage several bits of one byte. */
        for (size_t i = 0; i <= n; ++i) {
            unsigned a = (unsigned char)s->burst[0][i], b = (unsigned char)s->burst[1][i];
            unsigned c = (unsigned char)s->burst[2][i];
            s->burst[0][i] = (char)((a & b) | (a & c) | (b & c));
        }
        candidate = s->burst[0];
    }
    if (!candidate || s->published || !same_parse(candidate, &s->scratch)) return;
    s->published = true;
    if (s->have_active && !strcmp(s->active.header, candidate) && !s->active.ended) return;
    s->active = s->scratch; s->have_active = true;
    if (s->callback) s->callback(&s->active, s->user);
}
static void symbol(same_ctx_t *s, lane_t *l, int index, unsigned bit)
{
    l->shift = (uint8_t)((l->shift >> 1) | (bit << 7));
    if (!l->preamble) {
        if (l->shift == 0xab) {
            l->preamble = 1; l->bits = 0; l->confidence = 0; l->symbols = 0;
        }
        return;
    }
    if (++l->bits < 8) return;
    l->bits = 0;
    if (!l->len && l->shift == 0xab) { if (l->preamble < 16) ++l->preamble; return; }
    if (!l->len && (l->preamble < 8 || (l->shift != 'Z' && l->shift != 'N'))) {
        l->preamble = 0; return;
    }
    if (l->len >= SAME_HEADER_MAX - 1) { l->preamble = 0; l->len = 0; return; }
    l->text[l->len++] = (char)l->shift; l->text[l->len] = 0;
    if (l->len == 4 && !strcmp(l->text, "ZCZC")) {
        /* Let every phase lane finish the sync word before choosing timing. */
        float confidence = l->confidence / (l->symbols ? l->symbols : 1);
        if (!s->select_at) { s->select_at = s->samples + 32; s->best_confidence = -1; }
        if (confidence > s->best_confidence) {
            s->candidate = index; s->best_confidence = confidence;
        }
    }
    const char *plus = strchr(l->text, '+');
    if ((l->len == 4 && !strcmp(l->text, "NNNN")) ||
        (plus && l->len - (int)(plus - l->text) == 23)) {
        same_burst(s, l->text);
        s->locked = s->candidate = -1; s->select_at = 0;
        for (int k = 0; k < 8; ++k) {
            s->lanes[k].preamble = s->lanes[k].bits = 0;
            s->lanes[k].len = 0;
        }
    }
}
void same_process(same_ctx_t *s, const int16_t *pcm, int n)
{
    if (!s) return;
    /* Tone oscillators are shared; each phase lane integrates one symbol. */
    const float cm = 0.683592f, sm = 0.729857f; /* 2083 1/3 Hz at 16 kHz */
    const float cs = 0.817585f, ss = 0.575808f; /* 1562.5 Hz at 16 kHz */
    for (int j = 0; j < n; ++j) {
        float x = pcm[j] / 32768.0f; ++s->samples;
        if (s->select_at && s->samples >= s->select_at) {
            s->locked = s->candidate; s->select_at = 0;
        }
        if (abs(pcm[j]) < 100) { if (s->quiet <= 1600) ++s->quiet; }
        else s->quiet = 0;
        if (s->quiet == 1600) {
            if (s->locked >= 0) same_burst(s, s->lanes[s->locked].text);
            s->locked = s->candidate = -1; s->select_at = 0;
            for (int k = 0; k < 8; ++k) {
                s->lanes[k].preamble = s->lanes[k].bits = 0; s->lanes[k].len = 0;
            }
        }
        for (int k = 0; k < 8; ++k) {
            lane_t *l = &s->lanes[k];
            l->mi += x*s->mc; l->mq += x*s->ms;
            l->si += x*s->sc; l->sq += x*s->ss;
            l->phase += (3125.0f / 6.0f) / SAME_SAMPLE_RATE;
            if (l->phase >= 1) {
                l->phase -= 1;
                if (s->quiet < 1600 && (s->locked < 0 || s->locked == k)) {
                    float mark = l->mi*l->mi + l->mq*l->mq;
                    float space = l->si*l->si + l->sq*l->sq;
                    if (l->preamble) {
                        l->confidence += fabsf(mark - space) / (mark + space + 1e-12f);
                        ++l->symbols;
                    }
                    symbol(s, l, k, mark > space);
                }
                l->mi = l->mq = l->si = l->sq = 0;
            }
        }
        float a = s->mc*cm - s->ms*sm;
        s->ms = s->ms*cm + s->mc*sm; s->mc = a;
        a = s->sc*cs - s->ss*ss;
        s->ss = s->ss*cs + s->sc*ss; s->sc = a;
        /* Bound oscillator drift without expensive per-sample trigonometry. */
        if (!(s->samples & 255)) {
            float m = 1 / sqrtf(s->mc*s->mc + s->ms*s->ms);
            float t = 1 / sqrtf(s->sc*s->sc + s->ss*s->ss);
            s->mc *= m; s->ms *= m; s->sc *= t; s->ss *= t;
        }
    }
}
