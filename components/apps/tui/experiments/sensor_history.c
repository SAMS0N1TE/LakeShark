#include "sensor_history.h"
#include <math.h>
#include <string.h>

void sh_init(sh_store_t *s)
{
    memset(s, 0, sizeof(*s));
    s->show_strangers = true;
    s->days = 7;
}

static bool eq(float a, float b) { return a == b || (isnan(a) && isnan(b)); }
static bool same(const sh_reading_t *a, const sh_reading_t *b)
{
    return eq(a->temp_c, b->temp_c) && eq(a->humidity, b->humidity) &&
           eq(a->rain_mm, b->rain_mm) && eq(a->wind_ms, b->wind_ms) &&
           eq(a->kpa, b->kpa) && a->battery_ok == b->battery_ok;
}
const sh_reading_t *sh_at(const sh_sensor_t *s, unsigned i)
{
    if (i >= s->count) return NULL;
    return &s->ring[(s->head + SH_READINGS - s->count + i) % SH_READINGS];
}

void sh_expire(sh_store_t *s, int64_t now)
{
    for (int i = 0; i < SH_SENSORS; i++) {
        sh_sensor_t *d = &s->sensor[i];
        if (!d->used) continue;
        /* Hidden keys remain as a bounded deny list; naming pins a sensor. */
        if (!d->name[0] && !d->hidden && !d->own && now - d->last_seen >= SH_DAY) {
            memset(d, 0, sizeof(*d));
            continue;
        }
        while (d->count && now - sh_at(d, 0)->time >= s->days * SH_DAY) {
            unsigned at = (d->head + SH_READINGS - d->count) % SH_READINGS;
            memset(&d->ring[at], 0, sizeof(d->ring[at]));
            d->count--;
        }
    }
}

bool sh_threshold(sh_sensor_t *d, float c)
{
    if (!d->name[0] || !d->alert_on || !isfinite(c)) return false;
    if (d->alarm) {
        if (c <= d->threshold_c - 1.0f) d->alarm = false;
        return false;
    }
    if (c > d->threshold_c) { d->alarm = true; d->pending = true; return true; }
    return false;
}

void sh_own(sh_sensor_t *d, bool own)
{
    d->own = own && lr433_proto_class(d->proto) == LR433_TPMS;
    if (!d->own && lr433_proto_class(d->proto) == LR433_TPMS) {
        memset(d->ring, 0, sizeof(d->ring));
        d->count = d->head = 0;
        d->last.temp_c = d->last.kpa = d->raw.temp_c = d->raw.kpa = NAN;
        d->alarm = d->pending = false;
    }
}

int sh_receive(sh_store_t *s, const lr433_msg_t *m, int64_t now, bool *appended)
{
    *appended = false;
    if (lr433_proto_class(m->proto) == LR433_METER) return -1;
    sh_expire(s, now);
    int slot = -1, free_slot = -1, oldest = -1;
    for (int i = 0; i < SH_SENSORS; i++) {
        sh_sensor_t *d = &s->sensor[i];
        if (d->used && d->proto == m->proto && d->channel == m->channel && !strcmp(d->id, m->id)) {
            slot = i; break;
        }
        if (!d->used && free_slot < 0) free_slot = i;
        if (d->used && !d->name[0] && !d->hidden && !d->own &&
            (oldest < 0 || d->last_seen < s->sensor[oldest].last_seen)) oldest = i;
    }
    if (slot < 0) {
        slot = free_slot >= 0 ? free_slot : oldest;
        if (slot < 0) return -1;
        sh_sensor_t *d = &s->sensor[slot];
        memset(d, 0, sizeof(*d));
        d->used = true; d->proto = m->proto; d->channel = m->channel;
        snprintf(d->id, sizeof(d->id), "%s", m->id);
        d->last.temp_c = d->last.humidity = d->last.rain_mm = d->last.wind_ms = d->last.kpa = NAN;
        d->last.battery_ok = -1;
        d->threshold_c = -10;
    }
    sh_sensor_t *d = &s->sensor[slot];
    d->last_seen = now;
    /* Unowned TPMS retains identity only so the user can explicitly opt in. */
    if (d->hidden || (lr433_proto_class(m->proto) == LR433_TPMS && !d->own) ||
        lr433_proto_class(m->proto) == LR433_METER) return slot;
    sh_reading_t r = { .time = now, .temp_c = m->temp_c, .humidity = m->humidity,
        .rain_mm = m->rain_mm, .wind_ms = m->wind_ms, .kpa = m->kpa, .battery_ok = m->battery_ok };
    if (d->count && now - d->last_append >= 0 && now - d->last_append < 2 && same(&d->raw, &r)) return slot;
    d->raw = r;
    d->last_append = now;
    d->ring[d->head] = r;
    d->head = (d->head + 1) % SH_READINGS;
    if (d->count < SH_READINGS) d->count++;
    if (isfinite(r.temp_c)) d->last.temp_c = r.temp_c;
    if (isfinite(r.humidity)) d->last.humidity = r.humidity;
    if (isfinite(r.rain_mm)) d->last.rain_mm = r.rain_mm;
    if (isfinite(r.wind_ms)) d->last.wind_ms = r.wind_ms;
    if (isfinite(r.kpa)) d->last.kpa = r.kpa;
    if (r.battery_ok >= 0) d->last.battery_ok = r.battery_ok;
    d->last.time = now;
    sh_threshold(d, r.temp_c);
    *appended = true;
    return slot;
}

int sh_order(const sh_store_t *s, uint8_t out[SH_SENSORS])
{
    int n = 0;
    for (int i = 0; i < SH_SENSORS; i++) {
        const sh_sensor_t *d = &s->sensor[i];
        if (!d->used || d->hidden || (!s->show_strangers && !d->name[0])) continue;
        int at = n;
        while (at > 0) {
            const sh_sensor_t *p = &s->sensor[out[at - 1]];
            if ((!!p->name[0] > !!d->name[0]) ||
                (!!p->name[0] == !!d->name[0] && p->last_seen >= d->last_seen)) break;
            out[at] = out[at - 1]; at--;
        }
        out[at] = (uint8_t)i; n++;
    }
    return n;
}

static bool quoted(FILE *f, const char *s)
{
    if (fputc('"', f) == EOF) return false;
    for (; *s; s++) {
        if (*s == '"' && fputc('"', f) == EOF) return false;
        if (fputc(*s, f) == EOF) return false;
    }
    return fputc('"', f) != EOF;
}
bool sh_csv(FILE *f, const sh_sensor_t *s, const sh_reading_t *r)
{
    if (s->hidden || (lr433_proto_class(s->proto) == LR433_TPMS && !s->own)) return false;
    if (fprintf(f, "%lld,%u,", (long long)r->time, s->proto) < 0 || !quoted(f, s->id) ||
        fprintf(f, ",%d,", s->channel) < 0 || !quoted(f, s->name)) return false;
    return fprintf(f, ",%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", (double)r->temp_c,
        (double)r->humidity, (double)r->rain_mm, (double)r->wind_ms, (double)r->kpa,
        r->battery_ok < 0 ? -1 : !r->battery_ok) >= 0;
}
