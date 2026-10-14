#include "ls_survey.h"
#include <string.h>
#include <math.h>
#include <time.h>

void ls_survey_init(ls_survey_session_t *s, ls_survey_entry_t *table, unsigned cap, bool only_fix)
{
    *s = (ls_survey_session_t){.entries = table, .cap = cap > LS_SURVEY_CAP ? LS_SURVEY_CAP : cap,
                                .only_fix = only_fix};
}

bool ls_survey_fix_fresh(const ls_survey_fix_t *g, int64_t now)
{
    return g && g->fix && g->fix_us > 0 && now >= g->fix_us &&
        now - g->fix_us <= 10000000 && isfinite(g->lat) && isfinite(g->lon) &&
        g->lat >= -90 && g->lat <= 90 && g->lon >= -180 && g->lon <= 180;
}

bool ls_survey_note(ls_survey_session_t *s, const ls_survey_entry_t *o,
                    const ls_survey_fix_t *g, int64_t us, uint32_t ms)
{
    bool fresh = ls_survey_fix_fresh(g, us);
    if (s->only_fix && !fresh) return false;
    unsigned i;
    for (i = 0; i < s->count; i++) {
        ls_survey_entry_t *e = &s->entries[i];
        if (e->kind == o->kind && !memcmp(e->addr, o->addr, 6) &&
            (o->kind == LS_SURVEY_WIFI || e->addr_type == o->addr_type)) break;
    }
    bool added = i == s->count;
    if (!s->cap || !s->entries) return false;
    if (added && s->count == s->cap) {
        i = 0;
        for (unsigned j = 1; j < s->count; j++)
            if (ms - s->entries[j].last_ms > ms - s->entries[i].last_ms) i = j;
        if (s->summary && !ls_survey_summary(s->summary, &s->entries[i])) s->summary_error = true;
        s->dropped++; /* Evictions, not lost export rows. */
    }
    ls_survey_entry_t *e = &s->entries[i];
    if (added) {
        *e = *o;
        e->first_s = o->last_s;
        e->first_ms = ms;
        if (s->count < s->cap) s->count++;
        s->totals[o->kind]++;
        if (o->kind == LS_SURVEY_WIFI) {
            unsigned mask = ls_survey_security(o->auth);
            for (unsigned k = 0; k < 5; k++) if (mask & (1u << k)) s->security[k]++;
            if (o->channel < 15) s->channels[o->channel]++;
        }
        unsigned second = ms / 1000, bucket = second % 60;
        if (s->minute_stamp[bucket] != second) {
            s->minute_stamp[bucket] = second;
            s->minute[bucket][0] = s->minute[bucket][1] = 0;
        }
        s->minute[bucket][o->kind]++;
    }
    s->current = i;
    e->last_ms = ms;
    if (fresh) {
        double w = pow(10.0, fmax(-127, fmin(0, o->rssi)) / 10.0);
        double lat = g->lat * 0.017453292519943295, lon = g->lon * 0.017453292519943295;
        e->weight += w; e->x += w * cos(lat) * cos(lon);
        e->y += w * cos(lat) * sin(lon); e->z += w * sin(lat); e->samples++;
    }
    if (!e->epoch && o->epoch) { e->first_s = o->last_s; e->epoch = true; }
    e->last_s = o->last_s;
    if (o->name[0]) { memcpy(e->name, o->name, sizeof(e->name)); e->name[32] = 0; }
    if (added || o->rssi > e->rssi) {
        e->rssi = o->rssi;
        e->channel = o->channel;
        e->auth = o->auth;
        e->best_s = o->last_s;
        e->positioned = fresh;
        e->lat = fresh ? g->lat : 0;
        e->lon = fresh ? g->lon : 0;
    }
    return added;
}

static void quoted(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) { if (*s == '"') fputc('"', f); fputc((unsigned char)*s, f); }
    fputc('"', f);
}

/* IDF 5.5 wifi_auth_mode_t. Cipher suites are conventional estimates: the
 * scan bridge supplies authmode, not the RSN cipher/AKM list. */
const char *ls_survey_auth(unsigned auth)
{
    static const char *const modes[] = {
        "[ESS]", "[WEP][ESS]", "[WPA-PSK-TKIP][ESS]", "[WPA2-PSK-CCMP][ESS]",
        "[WPA-PSK-TKIP][WPA2-PSK-CCMP][ESS]", "[WPA2-EAP-CCMP][ESS]",
        "[WPA3-SAE-CCMP][ESS]", "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]",
        "[WAPI-PSK-SMS4][ESS]", "[OWE-CCMP][ESS]", "[WPA3-EAP-SUITE-B-192-GCMP-256][ESS]",
        "[WPA3-SAE-CCMP][ESS]", "[WPA3-SAE-CCMP][ESS]", "[DPP][ESS]",
        "[WPA3-EAP-CCMP][ESS]", "[WPA2-EAP-CCMP][WPA3-EAP-CCMP][ESS]", "[WPA-EAP-TKIP][ESS]"
    };
    return auth < sizeof(modes) / sizeof(modes[0]) ? modes[auth] : "[UNKNOWN][ESS]";
}

unsigned ls_survey_security(unsigned auth)
{
    /* Open, WEP, WPA, WPA2, WPA3; mixed modes contribute to both. */
    static const unsigned masks[] = {1,2,4,8,12,8,16,24,0,16,16,16,16,0,16,24,4};
    return auth < sizeof(masks) / sizeof(masks[0]) ? masks[auth] : 0;
}

bool ls_survey_header(FILE *f, const char *release, const char *board)
{
    if (!f) return false;
    fputs("WigleWifi-1.6,appRelease=", f); fputs(release, f);
    fputs(",model=ESP32-P4,release=ESP-IDF,device=LakeShark,display=TUI,board=", f);
    fputs(board, f);
    fputs(",brand=LakeShark,star=Sol,body=3,subBody=0\n"
          "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n", f);
    return !ferror(f);
}

static void address(FILE *f, const ls_survey_entry_t *e)
{
    fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x", e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5]);
}

bool ls_survey_row(FILE *f, const ls_survey_entry_t *o, const ls_survey_fix_t *g, uint32_t first_seen)
{
    if (!f || !o->epoch || first_seen < 1577836800u) return false;
    time_t t = first_seen;
    struct tm tm;
#ifdef _WIN32
    if (gmtime_s(&tm, &t)) return false;
#else
    if (!gmtime_r(&t, &tm)) return false;
#endif
    address(f, o); fputc(',', f); quoted(f, o->name);
    fprintf(f, ",%s,%04d-%02d-%02d %02d:%02d:%02d,%u,%u,%d,",
        o->kind == LS_SURVEY_WIFI ? ls_survey_auth(o->auth) : "[BLE]",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
        o->kind == LS_SURVEY_WIFI ? o->channel : 0,
        o->kind == LS_SURVEY_WIFI && o->channel >= 1 && o->channel <= 14 ?
            (o->channel == 14 ? 2484 : 2407 + o->channel * 5) : 0, o->rssi);
    if (g && g->fix) {
        fprintf(f, "%.7f,%.7f,", g->lat, g->lon);
        if (isfinite(g->altitude)) fprintf(f, "%.1f", (double)g->altitude);
        fputc(',', f);
        /* HDOP is dimensionless; assume 5 m UERE, not measured accuracy. */
        if (isfinite(g->hdop) && g->hdop > 0) fprintf(f, "%.1f", g->hdop * 5.0);
    } else fputs(",,,", f);
    fprintf(f, ",,,%s\n", o->kind == LS_SURVEY_WIFI ? "WIFI" : "BLE");
    return !ferror(f);
}

bool ls_survey_relog(const ls_survey_entry_t *e, const ls_survey_entry_t *o,
                     const ls_survey_fix_t *g, int64_t us)
{
    if (!e->logged || o->rssi >= e->row_rssi + 6) return true;
    if (!ls_survey_fix_fresh(g, us)) return false;
    if (!e->row_positioned) return true;
    double lat = (g->lat - e->row_lat) * 0.017453292519943295;
    double lon = remainder(g->lon - e->row_lon, 360.0) * 0.017453292519943295;
    double h = sin(lat / 2) * sin(lat / 2) + cos(g->lat * 0.017453292519943295) *
        cos(e->row_lat * 0.017453292519943295) * sin(lon / 2) * sin(lon / 2);
    return h >= 1.539801e-11; /* sin(50 / (2 * 6371000)) squared */
}

void ls_survey_logged(ls_survey_entry_t *e, const ls_survey_entry_t *o,
                      const ls_survey_fix_t *g, int64_t us)
{
    e->logged = true; e->row_rssi = o->rssi;
    e->row_positioned = ls_survey_fix_fresh(g, us);
    if (e->row_positioned) { e->row_lat = g->lat; e->row_lon = g->lon; }
}

bool ls_survey_centroid(const ls_survey_entry_t *e, double *lat, double *lon)
{
    if (e->weight <= 0) return false;
    *lat = atan2(e->z, hypot(e->x, e->y)) * 57.29577951308232;
    *lon = atan2(e->y, e->x) * 57.29577951308232;
    return true;
}

bool ls_survey_summary(FILE *f, const ls_survey_entry_t *e)
{
    double lat, lon;
    if (!f) return false;
    address(f, e); fprintf(f, ",%s,%u,", e->kind == LS_SURVEY_WIFI ? "WIFI" : "BLE", e->addr_type);
    quoted(f, e->name); fprintf(f, ",%lu,", (unsigned long)e->samples);
    if (ls_survey_centroid(e, &lat, &lon)) fprintf(f, "%.7f,%.7f", lat, lon);
    else fputc(',', f);
    fprintf(f, ",%.12g\n", e->weight);
    return !ferror(f);
}

bool ls_survey_filename(char *out, size_t cap, uint32_t seconds, bool epoch)
{
    int n;
    if (epoch) {
        time_t t = seconds;
        struct tm tm;
#ifdef _WIN32
        if (gmtime_s(&tm, &t)) return false;
#else
        if (!gmtime_r(&t, &tm)) return false;
#endif
        n = snprintf(out, cap, "%04d%02d%02d_%02d%02d%02d.csv", tm.tm_year + 1900,
                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else return false;
    return n >= 0 && (size_t)n < cap;
}

int ls_survey_compare(const ls_survey_entry_t *a, const ls_survey_entry_t *b, int sort)
{
    int d = sort == 0 ? b->rssi - a->rssi : sort == 1 ?
        (a->first_ms < b->first_ms ? 1 : a->first_ms > b->first_ms ? -1 : 0) : strcmp(a->name, b->name);
    if (d) return d;
    d = (int)a->kind - (int)b->kind;
    if (!d) d = memcmp(a->addr, b->addr, 6);
    return d ? d : (int)a->addr_type - (int)b->addr_type;
}

