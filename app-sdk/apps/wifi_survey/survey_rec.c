/* WIFI SURVEY's recorder core (survey_rec.h). The rules and the CSV are
   LINK's SURVEY's (ls_survey.c in the firmware until 2026-11-15), moved
   onto what an SD app has: float trig only, no stdio, no gmtime. */
#include "survey_rec.h"
#include "lap_dot11.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define M_PER_DEG 111194.9266       /* metres in a degree of latitude */
#define RELOG_M 50.0

static double wrap180(double d)
{
    if (d > 180) d -= 360;
    else if (d < -180) d += 360;
    return d;
}

void sr_init(sr_session_t *s, sr_entry_t *table, uint16_t *heads, unsigned cap, bool only_fix)
{
    *s = (sr_session_t){.e = table, .head = heads, .cap = cap > SR_CAP ? SR_CAP : cap, .only_fix = only_fix};
    if (heads) memset(heads, 0, sizeof(uint16_t) * SR_BUCKETS);
}

bool sr_fix_fresh(const sr_fix_t *g, int64_t now)
{
    return g && g->fix && now >= g->fix_ms && now - g->fix_ms <= SR_FRESH_MS &&
           isfinite(g->lat) && isfinite(g->lon) && g->lat >= -90 && g->lat <= 90 && g->lon >= -180 && g->lon <= 180;
}

static unsigned bucket(uint8_t kind, const uint8_t *a, uint8_t type)
{
    unsigned h = 2166136261u ^ kind ^ (unsigned)type << 8;
    for (int i = 0; i < 6; i++) h = (h ^ a[i]) * 16777619u;
    return (h ^ h >> 15) & (SR_BUCKETS - 1);
}
static bool same(const sr_entry_t *e, const sr_obs_t *o)
{
    return e->kind == o->kind && !memcmp(e->addr, o->addr, 6) && (o->kind == SR_WIFI || e->addr_type == o->addr_type);
}
static void unlink_entry(sr_session_t *s, unsigned i)
{
    const sr_entry_t *e = &s->e[i];
    uint16_t *p = &s->head[bucket(e->kind, e->addr, e->kind == SR_WIFI ? 0 : e->addr_type)];
    while (*p && *p != i + 1) p = &s->e[*p - 1].next;
    if (*p) *p = e->next;
}

int sr_note(sr_session_t *s, const sr_obs_t *o, const sr_fix_t *g, uint32_t utc, int64_t now)
{
    const bool fresh = sr_fix_fresh(g, now);
    if (s->only_fix && !fresh) return SR_IGNORED;
    if (!s->cap || !s->e || !s->head) return SR_IGNORED;
    const uint32_t ms = (uint32_t)now;
    const unsigned b = bucket(o->kind, o->addr, o->kind == SR_WIFI ? 0 : o->addr_type);
    unsigned i = SR_CAP;
    for (uint16_t k = s->head[b]; k; k = s->e[k - 1].next)
        if (same(&s->e[k - 1], o)) { i = k - 1u; break; }
    const bool added = i == SR_CAP;
    if (!added) {
        sr_entry_t *e = &s->e[i];
        e->last_ms = ms;
        /* One sighting a second counts: a beacon every 100 ms is not ten. */
        if (ms - e->sample_ms < SR_SAMPLE_MS && o->rssi <= e->rssi) return SR_IGNORED;
    } else if (s->count < s->cap) i = s->count++;
    else {
        /* Full: the one heard least recently goes, its summary row first. */
        i = 0;
        for (unsigned j = 1; j < s->count; j++)
            if (ms - s->e[j].last_ms > ms - s->e[i].last_ms) i = j;
        if (s->evicted) s->evicted(s->ctx, &s->e[i]);
        if (s->e[i].positioned && s->positioned) s->positioned--;
        unlink_entry(s, i);
        s->dropped++;               /* evictions, not lost rows */
    }
    sr_entry_t *e = &s->e[i];
    if (added) {
        memset(e, 0, sizeof(*e));
        e->kind = o->kind; memcpy(e->addr, o->addr, 6); e->addr_type = o->kind == SR_WIFI ? 0 : o->addr_type;
        e->first_s = utc;
        e->first_ms = ms;
        e->next = s->head[b];
        s->head[b] = (uint16_t)(i + 1);
        s->totals[o->kind == SR_WIFI ? 0 : 1]++;
        if (o->kind == SR_WIFI) {
            const unsigned mask = sr_security(o->sec);
            for (unsigned k = 0; k < 5; k++) if (mask & (1u << k)) s->security[k]++;
        }
        const uint32_t second = ms / 1000u, slot = second % 60u;
        if (s->minute_stamp[slot] != second) {
            s->minute_stamp[slot] = second;
            s->minute[slot][0] = s->minute[slot][1] = 0;
        }
        s->minute[slot][o->kind == SR_WIFI ? 0 : 1]++;
    }
    s->current = i;
    e->last_ms = e->sample_ms = ms;
    e->seen++;
    if (!e->first_s && utc) e->first_s = utc;
    if (fresh) {
        const int r = o->rssi < -127 ? -127 : o->rssi > 0 ? 0 : o->rssi;
        const double w = (double)powf(10.0f, (float)r / 10.0f);
        if (e->weight <= 0) { e->ref_lat = g->lat; e->ref_lon = g->lon; }
        e->weight += w;
        e->sum_lat += w * (g->lat - e->ref_lat);
        e->sum_lon += w * wrap180(g->lon - e->ref_lon);
        e->samples++;
    }
    if (o->name[0]) { memcpy(e->name, o->name, sizeof(e->name)); e->name[32] = 0; }
    if (added || o->rssi > e->rssi) {
        e->rssi = (int8_t)(o->rssi < -128 ? -128 : o->rssi > 127 ? 127 : o->rssi);
        e->channel = o->channel;
        e->sec = o->sec;
        if (e->positioned != fresh) {
            if (fresh) s->positioned++;
            else if (s->positioned) s->positioned--;
        }
        e->positioned = fresh;
        e->lat = fresh ? g->lat : 0;
        e->lon = fresh ? g->lon : 0;
    }
    return added ? SR_ADDED : SR_UPDATED;
}

static double metres(double lat0, double lon0, double lat1, double lon1)
{
    const double dy = (lat1 - lat0) * M_PER_DEG;
    const double dx = wrap180(lon1 - lon0) * M_PER_DEG * (double)cosf((float)((lat0 + lat1) * 0.00872664625997));
    return dy * dy + dx * dx;           /* squared */
}

bool sr_relog(const sr_entry_t *e, const sr_obs_t *o, const sr_fix_t *g, int64_t now)
{
    if (!e->logged || o->rssi >= e->row_rssi + 6) return true;
    if (!sr_fix_fresh(g, now)) return false;
    if (!e->row_positioned) return true;
    return metres(e->row_lat, e->row_lon, g->lat, g->lon) >= RELOG_M * RELOG_M;
}

void sr_logged(sr_entry_t *e, const sr_obs_t *o, const sr_fix_t *g, int64_t now)
{
    e->logged = true;
    e->row_rssi = (int8_t)(o->rssi < -128 ? -128 : o->rssi > 127 ? 127 : o->rssi);
    e->row_positioned = sr_fix_fresh(g, now);
    if (e->row_positioned) { e->row_lat = g->lat; e->row_lon = g->lon; }
}

bool sr_centroid(const sr_entry_t *e, double *lat, double *lon)
{
    if (e->weight <= 0) return false;
    *lat = e->ref_lat + e->sum_lat / e->weight;
    *lon = wrap180(e->ref_lon + e->sum_lon / e->weight);
    return true;
}

void sr_minute(const sr_session_t *s, int64_t now, unsigned out[2])
{
    const uint32_t second = (uint32_t)now / 1000u;
    out[0] = out[1] = 0;
    for (unsigned i = 0; i < 60; i++)
        if (second - s->minute_stamp[i] < 60u) { out[0] += s->minute[i][0]; out[1] += s->minute[i][1]; }
}

/* ---- text ---------------------------------------------------------------------- */

typedef struct { char *p; size_t cap, n; bool over; } sink_t;
static void put(sink_t *k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(sink_t *k, const char *fmt, ...)
{
    if (k->over) return;
    va_list a;
    va_start(a, fmt);
    const int n = vsnprintf(k->p + k->n, k->cap - k->n, fmt, a);
    va_end(a);
    if (n < 0 || (size_t)n >= k->cap - k->n) { k->over = true; return; }
    k->n += (size_t)n;
}
static int done(sink_t *k)
{
    if (k->over) { if (k->cap) k->p[0] = 0; return -1; }
    return (int)k->n;
}
static void quoted(sink_t *k, const char *s)
{
    put(k, "\"");
    for (; *s && !k->over; s++) {
        if (*s == '"') put(k, "\"\"");
        else put(k, "%c", *s);
    }
    put(k, "\"");
}
static void mac(sink_t *k, const uint8_t *a)
{
    put(k, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}

int sr_header(char *out, size_t cap, const char *release, const char *board)
{
    sink_t k = {out, cap, 0, !cap};
    put(&k, "WigleWifi-1.6,appRelease=%s,model=ESP32-P4,release=ESP-IDF,device=LakeShark,display=TUI,board=%s,"
            "brand=LakeShark,star=Sol,body=3,subBody=0\n"
            "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,"
            "AccuracyMeters,RCOIs,MfgrId,Type\n", release, board);
    return done(&k);
}

int sr_summary_header(char *out, size_t cap)
{
    sink_t k = {out, cap, 0, !cap};
    put(&k, "MAC,Type,AddressType,SSID,Samples,EstimatedLatitude,EstimatedLongitude,Weight\n");
    return done(&k);
}

/* Days from 1970-01-01 to y-m-d and back, proleptic Gregorian (H. Hinnant). */
bool sr_utc_text(char out[20], uint32_t t)
{
    const uint32_t days = t / 86400u, sod = t % 86400u;
    const int32_t z = (int32_t)days + 719468;
    const int32_t era = z / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    const int y = (int)yoe + era * 400 + (m <= 2);
    return snprintf(out, 20, "%04d-%02u-%02u %02u:%02u:%02u", y, m, d, (unsigned)(sod / 3600u),
                    (unsigned)(sod / 60u % 60u), (unsigned)(sod % 60u)) == 19;
}

int sr_row(char *out, size_t cap, const sr_obs_t *o, const sr_fix_t *g, uint32_t first_seen)
{
    char when[20];
    if (!cap) return -1;
    out[0] = 0;
    if (first_seen < SR_UTC_MIN || !sr_utc_text(when, first_seen)) return -1;
    const bool wifi = o->kind == SR_WIFI;
    sink_t k = {out, cap, 0, false};
    mac(&k, o->addr);
    put(&k, ",");
    quoted(&k, o->name);
    put(&k, ",%s,%s,%u,%u,%d,", wifi ? sr_auth(o->sec) : "[BLE]", when, wifi ? o->channel : 0u,
        wifi ? lap_dot11_channel_mhz(o->channel) : 0u, o->rssi);
    if (g && g->fix) {
        put(&k, "%.7f,%.7f,", g->lat, g->lon);
        if (isfinite(g->alt_m)) put(&k, "%.1f", (double)g->alt_m);
        put(&k, ",");
        /* HDOP is dimensionless; assume 5 m UERE, not measured accuracy. */
        if (isfinite(g->hdop) && g->hdop > 0) put(&k, "%.1f", (double)g->hdop * 5.0);
    } else put(&k, ",,,");
    put(&k, ",,,%s\n", wifi ? "WIFI" : "BLE");
    return done(&k);
}

int sr_summary(char *out, size_t cap, const sr_entry_t *e)
{
    double lat, lon;
    sink_t k = {out, cap, 0, !cap};
    mac(&k, e->addr);
    put(&k, ",%s,%u,", e->kind == SR_WIFI ? "WIFI" : "BLE", e->addr_type);
    quoted(&k, e->name);
    put(&k, ",%lu,", (unsigned long)e->samples);
    if (sr_centroid(e, &lat, &lon)) put(&k, "%.7f,%.7f", lat, lon);
    else put(&k, ",");
    put(&k, ",%.12g\n", e->weight);
    return done(&k);
}

bool sr_filename(char *out, size_t cap, uint32_t utc)
{
    char t[20];
    if (utc < SR_UTC_MIN || !sr_utc_text(t, utc)) return false;
    const int n = snprintf(out, cap, "%.4s%.2s%.2s_%.2s%.2s%.2s.csv", t, t + 5, t + 8, t + 11, t + 14, t + 17);
    return n > 0 && (size_t)n < cap;
}

bool sr_session_name(const char *name)
{
    const char *p = name;
    if (!strncmp(p, "undated_", 8)) {
        p += 8;
        for (int i = 0; i < 10; i++, p++) if (*p < '0' || *p > '9') return false;
    } else {
        for (int i = 0; i < 15; i++, p++) {
            if (i == 8) { if (*p != '_') return false; }
            else if (*p < '0' || *p > '9') return false;
        }
    }
    if (*p == '_') { p++; for (int i = 0; i < 3; i++, p++) if (*p < '0' || *p > '9') return false; }
    return !strcmp(p, ".csv");
}

const char *sr_auth(unsigned sec)
{
    switch (sec) {
    case LAP_DOT11_SEC_OPEN: return "[ESS]";
    case LAP_DOT11_SEC_WEP: return "[WEP][ESS]";
    case LAP_DOT11_SEC_WPA: return "[WPA-PSK-TKIP][ESS]";
    case LAP_DOT11_SEC_WPA2: return "[WPA2-PSK-CCMP][ESS]";
    case LAP_DOT11_SEC_WPA2_EAP: return "[WPA2-EAP-CCMP][ESS]";
    case LAP_DOT11_SEC_WPA2_WPA3: return "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]";
    case LAP_DOT11_SEC_WPA3: return "[WPA3-SAE-CCMP][ESS]";
    case LAP_DOT11_SEC_OWE: return "[OWE-CCMP][ESS]";
    default: return "[UNKNOWN][ESS]";
    }
}

unsigned sr_security(unsigned sec)
{
    switch (sec) {
    case LAP_DOT11_SEC_OPEN: return 1;
    case LAP_DOT11_SEC_WEP: return 2;
    case LAP_DOT11_SEC_WPA: return 4;
    case LAP_DOT11_SEC_WPA2: case LAP_DOT11_SEC_WPA2_EAP: return 8;
    case LAP_DOT11_SEC_WPA2_WPA3: return 24;
    case LAP_DOT11_SEC_WPA3: case LAP_DOT11_SEC_OWE: return 16;
    default: return 0;
    }
}

int sr_compare(const sr_entry_t *a, const sr_entry_t *b, int sort)
{
    int d = sort == 0 ? b->rssi - a->rssi : sort == 1 ?
        (a->first_ms < b->first_ms ? 1 : a->first_ms > b->first_ms ? -1 : 0) : strcmp(a->name, b->name);
    if (d) return d;
    d = (int)a->kind - (int)b->kind;
    if (!d) d = memcmp(a->addr, b->addr, 6);
    return d ? d : (int)a->addr_type - (int)b->addr_type;
}
