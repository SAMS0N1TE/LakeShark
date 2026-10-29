/* See ls_meters.h. */

#include "ls_meters.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_textpack.h"

void lsm_init(lsm_store_t *s)
{
    memset(s, 0, sizeof(*s));
}

int lsm_find(const lsm_store_t *s, uint8_t proto, const char *id)
{
    for (int i = 0; i < LSM_MAX; i++)
        if (s->m[i].used && s->m[i].proto == proto && !strcmp(s->m[i].id, id)) return i;
    return -1;
}

static int new_slot(lsm_store_t *s)
{
    int oldest = -1;
    for (int i = 0; i < LSM_MAX; i++) {
        if (!s->m[i].used) return i;
        if (s->m[i].pinned || s->m[i].name[0]) continue;
        if (oldest < 0 || s->m[i].last_s < s->m[oldest].last_s) oldest = i;
    }
    return oldest;
}

static lsm_meter_t *claim(lsm_store_t *s, uint8_t proto, const char *id)
{
    int i = lsm_find(s, proto, id);
    if (i >= 0) return &s->m[i];
    i = new_slot(s);
    if (i < 0) return NULL;
    lsm_meter_t *m = &s->m[i];
    memset(m, 0, sizeof(*m));
    m->used = true;
    m->proto = proto;
    m->value = -1;
    m->rssi = NAN;
    snprintf(m->id, sizeof(m->id), "%s", id);
    return m;
}

static void append(lsm_meter_t *m, int64_t t, int64_t v)
{
    const int at = (m->head + m->count) % LSM_HIST;
    m->hist[at].t = t;
    m->hist[at].v = v;
    if (m->count < LSM_HIST) m->count++;
    else m->head = (uint8_t)((m->head + 1) % LSM_HIST);
}

const lsm_point_t *lsm_point(const lsm_meter_t *m, int i)
{
    return &m->hist[(m->head + i) % LSM_HIST];
}

int lsm_receive(lsm_store_t *s, const lr433_msg_t *msg, uint32_t hz, float rssi,
                uint8_t source, int64_t now_s)
{
    if (!s || !msg || lr433_proto_class(msg->proto) != LR433_METER || !msg->id[0]) return -1;
    lsm_meter_t *m = claim(s, msg->proto, msg->id);
    if (!m) return -1;
    if (!m->first_s) m->first_s = now_s;
    m->last_s = now_s;
    m->heard++;
    m->hz = hz;
    m->rssi = rssi;
    m->source = source;
    if (msg->consumption >= 0) {
        const lsm_point_t *last = m->count ? lsm_point(m, m->count - 1) : NULL;
        /* A change is a point; so is the same reading after a quiet spell. */
        if (!last || last->v != msg->consumption || now_s - last->t >= LSM_FLAT_S)
            append(m, now_s, msg->consumption);
        m->value = msg->consumption;
    }
    s->seq++;
    return (int)(m - s->m);
}

int lsm_order(const lsm_store_t *s, uint8_t out[LSM_MAX])
{
    bool taken[LSM_MAX] = { false };
    int n = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < LSM_MAX; i++) {
            const lsm_meter_t *a = &s->m[i];
            if (!a->used || taken[i]) continue;
            if (best < 0) { best = i; continue; }
            const lsm_meter_t *b = &s->m[best];
            if (a->pinned != b->pinned ? a->pinned : a->last_s > b->last_s) best = i;
        }
        if (best < 0) break;
        taken[best] = true;
        out[n++] = (uint8_t)best;
    }
    return n;
}

/* The first point since the counter last went backwards (a reset, or a
   different meter answering to the same ID). */
static int since_reset(const lsm_meter_t *m)
{
    int from = 0;
    for (int i = 1; i < m->count; i++)
        if (lsm_point(m, i)->v < lsm_point(m, i - 1)->v) from = i;
    return from;
}

double lsm_rate_per_hour(const lsm_meter_t *m)
{
    if (!m || m->count < 1 || m->value < 0) return NAN;
    const lsm_point_t *a = lsm_point(m, since_reset(m));
    const int64_t dt = m->last_s - a->t;
    if (dt < 60 || m->value < a->v) return NAN;
    return (double)(m->value - a->v) * 3600.0 / (double)dt;
}

/* The reading at time t, by straight lines between points, held after the
   last one. */
static double reading_at(const lsm_meter_t *m, int from, int64_t t)
{
    const lsm_point_t *p = lsm_point(m, from);
    if (t <= p->t) return (double)p->v;
    for (int i = from + 1; i < m->count; i++) {
        const lsm_point_t *q = lsm_point(m, i);
        if (t <= q->t) return q->t == p->t ? (double)q->v
                                           : p->v + (double)(q->v - p->v) * (double)(t - p->t) / (double)(q->t - p->t);
        p = q;
    }
    return (double)p->v;
}

int64_t lsm_usage(const lsm_meter_t *m, int buckets, float *out)
{
    if (!m || !out || buckets <= 0) return 0;
    for (int i = 0; i < buckets; i++) out[i] = 0;
    if (m->count < 2) return 0;
    const int from = since_reset(m);
    const int64_t t0 = lsm_point(m, from)->t;
    int64_t t1 = lsm_point(m, m->count - 1)->t;
    if (m->last_s > t1) t1 = m->last_s;
    if (t1 <= t0 || m->count - from < 2) return 0;
    const int64_t span = (t1 - t0 + buckets - 1) / buckets;
    if (span <= 0) return 0;
    double prev = reading_at(m, from, t0);
    for (int b = 0; b < buckets; b++) {
        const double next = reading_at(m, from, t0 + span * (b + 1));
        out[b] = (float)(next - prev);
        prev = next;
    }
    return span;
}

bool lsm_pin(lsm_store_t *s, int slot, bool pin)
{
    if (!s || slot < 0 || slot >= LSM_MAX || !s->m[slot].used) return false;
    s->m[slot].pinned = pin;
    s->pins_dirty = true;
    s->seq++;
    return true;
}

static void clean_name(char *out, size_t n, const char *in)
{
    size_t k = 0;
    for (; in && *in && k + 1 < n; in++)
        if (*in >= ' ' && *in <= '~' && *in != '"' && *in != '\\') out[k++] = *in;
    out[k] = 0;
}

bool lsm_name(lsm_store_t *s, int slot, const char *name)
{
    if (!s || slot < 0 || slot >= LSM_MAX || !s->m[slot].used) return false;
    clean_name(s->m[slot].name, sizeof(s->m[slot].name), name);
    s->pins_dirty = true;
    s->seq++;
    return true;
}

void lsm_clear(lsm_store_t *s)
{
    for (int i = 0; i < LSM_MAX; i++)
        if (s->m[i].used && !s->m[i].pinned && !s->m[i].name[0]) memset(&s->m[i], 0, sizeof(s->m[i]));
    s->seq++;
}

/* ------------------------------------------------------------ the file -- */

size_t lsm_pins_json(const lsm_store_t *s, char *out, size_t n)
{
    if (!s || !out || n < 32) return 0;
    size_t at = (size_t)snprintf(out, n, "{\"v\":1,\"meters\":[");
    bool first = true;
    for (int i = 0; i < LSM_MAX; i++) {
        const lsm_meter_t *m = &s->m[i];
        if (!m->used || (!m->pinned && !m->name[0])) continue;
        const int w = LS_SNPRINTF(t_ls_meters_s_type_s_id, out + at, n - at, "%s\n {\"type\":\"%s\",\"id\":\"%s\",\"name\":\"%s\",\"pin\":%d}",
                               first ? "" : ",", lr433_proto_name(m->proto), m->id, m->name, m->pinned ? 1 : 0);
        if (w < 0 || (size_t)w >= n - at) return 0;
        at += (size_t)w;
        first = false;
    }
    const int w = snprintf(out + at, n - at, "\n]}\n");
    if (w < 0 || (size_t)w >= n - at) return 0;
    return at + (size_t)w;
}

/* The string value of "key" inside [p, end), unescaped only as far as this
   schema needs (it writes no escapes). */
static bool field(const char *p, const char *end, const char *key, char *out, size_t n)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const size_t pl = strlen(pat);
    for (const char *q = p; q + pl <= end; q++) {
        if (memcmp(q, pat, pl)) continue;
        q += pl;
        while (q < end && (*q == ' ' || *q == ':' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= end) return false;
        size_t k = 0;
        if (*q == '"') {
            for (q++; q < end && *q != '"' && k + 1 < n; q++) out[k++] = *q;
        } else {
            for (; q < end && (*q == '-' || (*q >= '0' && *q <= '9')) && k + 1 < n; q++) out[k++] = *q;
        }
        out[k] = 0;
        return true;
    }
    return false;
}

static int proto_named(const char *name)
{
    for (int p = 0; p < LR433_P_COUNT; p++)
        if (lr433_proto_class(p) == LR433_METER && !strcmp(lr433_proto_name(p), name)) return p;
    return -1;
}

int lsm_pins_load(lsm_store_t *s, const char *json)
{
    if (!s || !json) return -1;
    const char *end = json + strlen(json);
    char v[8];
    const char *list = strstr(json, "\"meters\"");
    if (!field(json, list ? list : end, "v", v, sizeof(v)) || strcmp(v, "1") || !list) return -1;
    int applied = 0;
    for (const char *p = strchr(list, '{'); p && p < end; p = strchr(p + 1, '{')) {
        const char *close = strchr(p, '}');
        if (!close) break;
        char type[24], id[16], name[LSM_NAME + 8] = "", pin[8] = "0";
        if (!field(p, close, "type", type, sizeof(type)) || !field(p, close, "id", id, sizeof(id))) continue;
        field(p, close, "name", name, sizeof(name));
        field(p, close, "pin", pin, sizeof(pin));
        const int proto = proto_named(type);
        if (proto < 0 || !id[0]) continue;
        lsm_meter_t *m = claim(s, (uint8_t)proto, id);
        if (!m) continue;
        m->pinned = atoi(pin) != 0;
        clean_name(m->name, sizeof(m->name), name);
        applied++;
    }
    s->seq++;
    return applied;
}
