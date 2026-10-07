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
    if (added && (s->count == s->cap || !s->entries)) { s->dropped++; return false; }
    ls_survey_entry_t *e = &s->entries[i];
    if (added) {
        *e = *o;
        e->first_s = o->last_s;
        e->first_ms = ms;
        s->count++;
    }
    e->last_s = o->last_s;
    if (o->name[0]) { memcpy(e->name, o->name, sizeof(e->name)); e->name[32] = 0; }
    if (added || o->rssi > e->rssi) {
        e->rssi = o->rssi;
        e->channel = o->channel;
        e->auth = o->auth;
        e->best_s = o->last_s;
        e->epoch = o->epoch;
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

bool ls_survey_csv(FILE *f, const ls_survey_session_t *s)
{
    if (!f) return false;
    fputs("kind,name,address,address_type,channel,auth,rssi,first_s,last_s,best_s,time_basis,latitude,longitude,note (random BLE addresses rotate)\n", f);
    for (unsigned i = 0; i < s->count; i++) {
        const ls_survey_entry_t *e = &s->entries[i];
        fprintf(f, "%s,", e->kind == LS_SURVEY_WIFI ? "wifi" : "ble");
        quoted(f, e->name);
        fprintf(f, ",%02x:%02x:%02x:%02x:%02x:%02x,", e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5]);
        if (e->kind == LS_SURVEY_BLE) fprintf(f, "%u,,", e->addr_type);
        else fprintf(f, ",%u", e->channel);
        if (e->kind == LS_SURVEY_WIFI) fprintf(f, ",%u", e->auth);
        fprintf(f, ",%d,%lu,%lu,%lu,%s,", e->rssi, (unsigned long)e->first_s,
                (unsigned long)e->last_s, (unsigned long)e->best_s, e->epoch ? "utc" : "uptime");
        if (e->positioned) fprintf(f, "%.7f,%.7f", e->lat, e->lon);
        else fputc(',', f);
        fprintf(f, ",%s\n", e->kind == LS_SURVEY_BLE ? "Random BLE addresses rotate; counts are addresses" : "");
    }
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
    } else n = snprintf(out, cap, "undated_%010lu.csv", (unsigned long)seconds);
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

