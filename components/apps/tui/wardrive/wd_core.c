/* WARDRIVE's session (wd_core.h). Plain C and stdio: the same file runs on
   the board's worker task and in the simulator's frame. */
#include "wd_core.h"
#include "wd_feed.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include <unistd.h>
#endif
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define M_PER_DEG 111194.9266
#define RELOG_M 50.0
#define BUCKETS 1024u
#define FRESH_MS 10000u
#define UTC_MIN 1577836800L         /* 2020-01-01: older is a clock never set */

static EXT_RAM_BSS_ATTR wd_stats_t S;
static wd_dev_t *D;
static uint16_t *H;
static int nd;
static wd_pt_t *T;
static int nt;
static float t_step = 5.0f;
static EXT_RAM_BSS_ATTR wd_drop_t R[WD_DROPS];
static int r_head, r_count;
static char *rows;
static size_t rows_n;
static int req;
/* Worker-only: the live log, and the locker queue. */
static FILE *csv_f;
static uint32_t csv_flush_ms;
static bool gpx_pending, gpx_sent;
static bool s_saving;            /* under wd_lock: the worker is writing the stopped drive's export */

/* ------------------------------------------------------------ helpers */

static unsigned bucket(uint8_t kind, const uint8_t *a)
{
    unsigned h = 2166136261u ^ kind;
    for (int i = 0; i < 6; i++) h = (h ^ a[i]) * 16777619u;
    return (h ^ h >> 15) & (BUCKETS - 1);
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S.note, sizeof(S.note), fmt, ap);
    va_end(ap);
}

bool wd_utc_text(char out[20], uint32_t t)
{
    /* Days from 1970-01-01 to y-m-d, proleptic Gregorian (H. Hinnant). */
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

int wd_sec_class(unsigned a)
{
    switch (a) {
    case WD_AUTH_OPEN: case WD_AUTH_UNKNOWN: return WD_SEC_OPEN;
    case WD_AUTH_WEP: return WD_SEC_WEP;
    case WD_AUTH_WPA: case WD_AUTH_WAPI: return WD_SEC_WPA;
    case WD_AUTH_WPA3: case WD_AUTH_WPA2_WPA3: case WD_AUTH_OWE: case WD_AUTH_WPA3_EAP: return WD_SEC_WPA3;
    default: return WD_SEC_WPA2;
    }
}

const char *wd_auth_name(unsigned a)
{
    switch (a) {
    case WD_AUTH_OPEN: return "[ESS]";
    case WD_AUTH_WEP: return "[WEP][ESS]";
    case WD_AUTH_WPA: return "[WPA-PSK-TKIP][ESS]";
    case WD_AUTH_WPA_WPA2: return "[WPA-PSK-CCMP+TKIP][WPA2-PSK-CCMP+TKIP][ESS]";
    case WD_AUTH_WPA2: return "[WPA2-PSK-CCMP][ESS]";
    case WD_AUTH_WPA2_EAP: return "[WPA2-EAP-CCMP][ESS]";
    case WD_AUTH_WPA3: return "[WPA3-SAE-CCMP][ESS]";
    case WD_AUTH_WPA2_WPA3: return "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]";
    case WD_AUTH_OWE: return "[OWE-CCMP][ESS]";
    case WD_AUTH_WAPI: return "[WAPI-PSK][ESS]";
    case WD_AUTH_WPA3_EAP: return "[WPA3-EAP-SUITE-B-192][ESS]";
    default: return "[ESS]";
    }
}

const char *wd_auth_short(unsigned a)
{
    static const char *const N[] = { "OPEN", "WEP", "WPA", "WPA/2", "WPA2", "EAP", "WPA3", "WPA2/3",
                                     "OWE", "WAPI", "EAP3", "?" };
    return a <= WD_AUTH_UNKNOWN ? N[a] : "?";
}

void wd_local(double lat, double lon, float *x, float *z)
{
    double dl = lon - S.lon0;
    if (dl > 180) dl -= 360;
    else if (dl < -180) dl += 360;
    *x = (float)(dl * S.m_lon);
    *z = (float)(-(lat - S.lat0) * S.m_lat);
}

/* ------------------------------------------------------------ session */

bool wd_begin(uint32_t now, const bool src[WD_KINDS], bool only_fix)
{
    /* A START right after STOP (a bounced double tap) would clear the rows,
       the track and the file name the queued export is about to write. */
    if ((req & WD_REQ_EXPORT) || s_saving || (csv_f && !S.running)) {
        say("saving the last drive; START again in a moment");
        return false;
    }
    if (!D) {
        D = wd_feed_alloc(sizeof(wd_dev_t) * WD_CAP);
        H = wd_feed_alloc(sizeof(uint16_t) * BUCKETS);
        T = wd_feed_alloc(sizeof(wd_pt_t) * WD_TRAIL);
        rows = wd_feed_alloc(WD_ROWS_BUF);
        if (!D || !H || !T || !rows) { say("not enough memory for a session"); return false; }
    }
    memset(H, 0, sizeof(uint16_t) * BUCKETS);
    nd = nt = 0;
    t_step = 5.0f;
    r_head = r_count = 0;
    rows_n = 0;
    req = 0;
    gpx_pending = gpx_sent = false;
    const long base = S.utc_base;
    const wd_fix_t fix = S.fix;
    const uint32_t fix_ms = S.fix_ms;
    memset(&S, 0, sizeof(S));
    S.utc_base = base;               /* the clock outlives a session */
    S.fix = fix;
    S.fix_ms = fix_ms;
    S.heading = -1;
    S.running = S.ever = true;
    S.only_fix = only_fix;
    S.start_ms = now;
    S.card = 1;
    for (int k = 0; k < WD_KINDS; k++) S.src[k] = src[k];
    if (fix.fix && fix_ms) {
        S.origin = true;
        S.lat0 = fix.lat; S.lon0 = fix.lon;
        S.m_lat = (float)M_PER_DEG;
        S.m_lon = (float)(M_PER_DEG * cos(fix.lat * M_PI / 180));
    }
    return true;
}

bool wd_begin_replay(uint32_t now)
{
    const bool all[WD_KINDS] = { true, true, true, true };
    const wd_fix_t keep = S.fix;
    const uint32_t keep_ms = S.fix_ms;
    S.fix.fix = false;                 /* the drive's first fix sets the origin, not where the board is now */
    S.fix_ms = 0;
    if (!wd_begin(now, all, false)) { S.fix = keep; S.fix_ms = keep_ms; return false; }
    S.replay = true;
    S.utc_base = 0;
    return true;
}

void wd_set_sources(const bool src[WD_KINDS], bool only_fix)
{
    for (int k = 0; k < WD_KINDS; k++) S.src[k] = src[k];
    S.only_fix = only_fix;
}

void wd_end(uint32_t now)
{
    if (!S.running) return;
    S.running = false;
    S.stop_ms = now;
}

const wd_stats_t *wd_stats(void) { return &S; }
int wd_dev_count(void) { return nd; }
const wd_dev_t *wd_dev_at(int i) { return D && i >= 0 && i < nd ? &D[i] : NULL; }
int wd_trail_count(void) { return nt; }
const wd_pt_t *wd_trail_at(int i) { return T && i >= 0 && i < nt ? &T[i] : NULL; }
const wd_drop_t *wd_drop_at(int i)
{
    return i >= 0 && i < r_count ? &R[(r_head - 1 - i + 2 * WD_DROPS) % WD_DROPS] : NULL;
}
void wd_request(int r) { req |= r; }

unsigned wd_new_last_minute(uint32_t now)
{
    unsigned n = 0;
    const uint32_t s = now / 1000;
    for (int i = 0; i < 60; i++)
        if (S.sec_stamp[i] && s - S.sec_stamp[i] < 60) n += S.sec_new[i];
    return n;
}

unsigned wd_new_in_minute(uint32_t now, int ago)
{
    const uint32_t m = now / 60000 - (uint32_t)ago;
    const int slot = (int)(m % 30);
    return ago >= 0 && ago < 30 && S.min_stamp[slot] == m + 1 ? S.min_new[slot] : 0;
}

static void count_new(uint32_t now)
{
    const uint32_t s = now / 1000, m = now / 60000;
    const int a = (int)(s % 60), b = (int)(m % 30);
    if (S.sec_stamp[a] != s) { S.sec_stamp[a] = s; S.sec_new[a] = 0; }
    if (S.sec_new[a] < 65535) S.sec_new[a]++;
    if (S.min_stamp[b] != m + 1) { S.min_stamp[b] = m + 1; S.min_new[b] = 0; }
    if (S.min_new[b] < 65535) S.min_new[b]++;
}

static void trail_add(double lat, double lon, float x, float z, float alt, uint32_t now)
{
    if (nt == WD_TRAIL) {                    /* full: keep every other point, twice as far apart */
        for (int i = 1; i < WD_TRAIL / 2; i++) T[i] = T[2 * i];
        nt = WD_TRAIL / 2;
        t_step *= 2;
    }
    T[nt++] = (wd_pt_t){ lat, lon, x, z, alt, S.speed, now };
}

void wd_fix_note(const wd_fix_t *f, uint32_t now)
{
    if (f->utc >= UTC_MIN) S.utc_base = (long)f->utc - (long)(now / 1000);
    S.fix.compass_deg = f->compass_deg;
    if (!f->fix || !isfinite(f->lat) || !isfinite(f->lon)) {
        S.fix.fix = false;
        if (f->compass_deg >= 0 && S.speed < 1.0f) S.heading = f->compass_deg;
        return;
    }
    S.fix = *f;
    S.fix_ms = now ? now : 1;
    if (!S.origin) {
        S.origin = true;
        S.lat0 = f->lat; S.lon0 = f->lon;
        S.m_lat = (float)M_PER_DEG;
        S.m_lon = (float)(M_PER_DEG * cos(f->lat * M_PI / 180));
    }
    float x, z;
    wd_local(f->lat, f->lon, &x, &z);
    if (f->speed_mps >= 0) S.speed = f->speed_mps;
    if (S.speed > 1.0f && f->course_deg >= 0) S.heading = f->course_deg;
    else if (f->compass_deg >= 0) S.heading = f->compass_deg;
    if (S.running) {
        if (!nt) trail_add(f->lat, f->lon, x, z, f->alt_m, now);
        else {
            const wd_pt_t *l = &T[nt - 1];
            const float d = hypotf(x - l->x, z - l->z);
            /* Standing still the fix wanders a few metres: that is not driving. */
            if (d >= t_step && (S.speed > 0.4f || d >= 15.0f)) {
                if (d > 300.0f && now - l->ms < 3000) return;   /* a jump no car makes: a bad fix */
                S.dist_m += d;
                trail_add(f->lat, f->lon, x, z, f->alt_m, now);
                if (f->speed_mps < 0 && now > l->ms) S.speed = d * 1000.0f / (float)(now - l->ms);
            }
        }
    }
    S.x = x; S.z = z;
}

/* ------------------------------------------------------------ CSV */

typedef struct { char *p; size_t cap, n; bool over; } sink_t;

static void put(sink_t *k, const char *fmt, ...)
{
    if (k->over) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(k->p + k->n, k->cap - k->n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= k->cap - k->n) { k->over = true; return; }
    k->n += (size_t)w;
}

static void quoted(sink_t *k, const char *s)
{
    put(k, "\"");
    for (; *s && !k->over; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"') put(k, "\"\"");
        else put(k, "%c", c < 32 ? ' ' : c);
    }
    put(k, "\"");
}

int wd_csv_header(char *out, size_t cap, const char *board)
{
    sink_t k = { out, cap, 0, !cap };
    put(&k, "WigleWifi-1.4,appRelease=WARDRIVE-1.0,model=ESP32-P4,release=ESP-IDF,device=LakeShark,"
            "display=TUI,board=%s,brand=LakeShark\n"
            "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,"
            "AccuracyMeters,Type\n", board && *board ? board : "LakeShark");
    return k.over ? -1 : (int)k.n;
}

/* One row: this sighting's signal, at the position given. */
static int row(char *out, size_t cap, const wd_dev_t *e, int rssi, double lat, double lon, float alt, float hdop)
{
    char when[20];
    const long first = S.utc_base + (long)(e->first_ms / 1000);
    if (S.utc_base < UTC_MIN / 2 || first < UTC_MIN || !wd_utc_text(when, (uint32_t)first)) return -1;
    const bool wifi = e->kind == WD_WIFI;
    sink_t k = { out, cap, 0, false };
    put(&k, "%02x:%02x:%02x:%02x:%02x:%02x,", e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5]);
    quoted(&k, e->name);
    put(&k, ",%s,%s,%u,%d,%.7f,%.7f,", wifi ? wd_auth_name(e->auth) : "Misc [LE]", when,
        wifi ? (unsigned)e->channel : 0u, rssi, lat, lon);
    if (isfinite(alt)) put(&k, "%.1f", (double)alt);
    put(&k, ",");
    /* HDOP is dimensionless; 5 m a unit is an assumption, not a measurement. */
    if (isfinite(hdop) && hdop > 0) put(&k, "%.1f", (double)hdop * 5.0);
    put(&k, ",%s\n", wifi ? "WIFI" : "BLE");
    return k.over ? -1 : (int)k.n;
}

static void row_queue(wd_dev_t *e, int rssi)
{
    char line[256];
    const int n = row(line, sizeof(line), e, rssi, S.fix.lat, S.fix.lon, S.fix.alt_m, S.fix.hdop);
    if (n <= 0) return;
    if (rows_n + (size_t)n > WD_ROWS_BUF) { S.rows_lost++; return; }
    memcpy(rows + rows_n, line, (size_t)n);
    rows_n += (size_t)n;
    e->flags |= WD_F_ROW;
    e->row_rssi = (int8_t)rssi;
    e->row_lat = S.fix.lat;
    e->row_lon = S.fix.lon;
    S.rows++;
}

size_t wd_take_rows(char *out, size_t cap)
{
    if (!rows_n) return 0;
    size_t n = rows_n <= cap ? rows_n : 0;
    if (!n) {                                 /* hand over whole lines only */
        for (size_t i = cap; i > 0; i--) if (rows[i - 1] == '\n') { n = i; break; }
        if (!n) return 0;
    }
    memcpy(out, rows, n);
    memmove(rows, rows + n, rows_n - n);
    rows_n -= n;
    return n;
}

/* ------------------------------------------------------------ noting */

static int find(uint8_t kind, const uint8_t *a)
{
    for (uint16_t k = H[bucket(kind, a)]; k; k = D[k - 1].next)
        if (D[k - 1].kind == kind && !memcmp(D[k - 1].addr, a, 6)) return k - 1;
    return -1;
}

static void unlink_dev(int i)
{
    uint16_t *p = &H[bucket(D[i].kind, D[i].addr)];
    while (*p && *p != i + 1) p = &D[*p - 1].next;
    if (*p) *p = D[i].next;
}

static int fresh_slot(void)
{
    if (nd < WD_CAP) return nd++;
    int old = 0;                              /* full: the one heard least recently goes */
    for (int i = 1; i < nd; i++) if ((int32_t)(D[i].last_ms - D[old].last_ms) < 0) old = i;
    unlink_dev(old);
    if (D[old].flags & WD_F_PLACED && S.placed) S.placed--;
    S.evicted++;
    return old;
}

int wd_note(const wd_obs_t *o, uint32_t now)
{
    if (!S.running || !D || o->kind >= WD_KINDS || !S.src[o->kind]) return -1;
    const bool fresh = S.fix_ms && S.fix.fix && now - S.fix_ms <= FRESH_MS;
    if (S.only_fix && !fresh) return -1;
    int i = find(o->kind, o->addr);
    bool added = false;
    wd_dev_t *e;
    if (i < 0) {
        i = fresh_slot();
        e = &D[i];
        memset(e, 0, sizeof(*e));
        e->kind = o->kind;
        memcpy(e->addr, o->addr, 6);
        e->first_ms = now;
        e->rssi = e->row_rssi = o->rssi;
        e->auth = o->auth;
        e->channel = o->channel;
        snprintf(e->name, sizeof(e->name), "%s", o->name);
        const unsigned b = bucket(e->kind, e->addr);
        e->next = H[b];
        H[b] = (uint16_t)(i + 1);
        S.total[o->kind]++;
        if (o->kind == WD_WIFI) S.sec[wd_sec_class(o->auth)]++;
        count_new(now);
        wd_drop_t *d = &R[r_head];
        d->kind = o->kind;
        d->sec = (uint8_t)(o->kind == WD_WIFI ? wd_sec_class(o->auth) : 0);
        d->rssi = o->rssi;
        d->lane = (uint8_t)(bucket(o->kind, o->addr) * 7u);
        d->ms = now;
        snprintf(d->name, sizeof(d->name), "%s", o->name);
        r_head = (r_head + 1) % WD_DROPS;
        if (r_count < WD_DROPS) r_count++;
        added = true;
    } else {
        e = &D[i];
        if (now - e->sample_ms < 1000 && o->rssi <= e->last_rssi) return 0;   /* once a second is plenty */
        if (!e->name[0] && o->name[0]) snprintf(e->name, sizeof(e->name), "%s", o->name);
        if (o->kind == WD_WIFI && o->auth != WD_AUTH_UNKNOWN) e->auth = o->auth;
        if (o->channel) e->channel = o->channel;
    }
    e->sample_ms = e->last_ms = now;
    e->last_rssi = o->rssi;
    e->seen++;
    const bool have = o->own_pos || fresh;
    const bool stronger = added || o->rssi > e->rssi;
    if (have && (stronger || !(e->flags & WD_F_PLACED))) {
        e->lat = o->own_pos ? o->lat : S.fix.lat;
        e->lon = o->own_pos ? o->lon : S.fix.lon;
        e->alt = S.fix.alt_m;
        if (S.origin) wd_local(e->lat, e->lon, &e->x, &e->z);
        if (!(e->flags & WD_F_PLACED)) S.placed++;
        e->flags |= WD_F_PLACED;
    }
    if (stronger) e->rssi = o->rssi;
    if ((o->kind == WD_WIFI || o->kind == WD_BLE) && fresh && S.utc_base && !S.replay) {
        bool want = !(e->flags & WD_F_ROW) || o->rssi >= e->row_rssi + 6;
        if (!want) {
            float ax, az, bx, bz;
            wd_local(e->row_lat, e->row_lon, &ax, &az);
            wd_local(S.fix.lat, S.fix.lon, &bx, &bz);
            want = hypotf(ax - bx, az - bz) >= RELOG_M;
        }
        if (want) row_queue(e, o->rssi);
    }
    return added ? 1 : 0;
}

/* ------------------------------------------------------------ files */

static void stamp_make(void)
{
    if (S.stamp[0]) return;
    char t[20];
    if (S.utc_base >= UTC_MIN / 2 && wd_utc_text(t, (uint32_t)(S.utc_base + (long)(S.start_ms / 1000))))
        snprintf(S.stamp, sizeof(S.stamp), "WD_%.4s%.2s%.2s_%.2s%.2s%.2s", t, t + 5, t + 8, t + 11, t + 14, t + 17);
    else
        snprintf(S.stamp, sizeof(S.stamp), "WD_up%06lu", (unsigned long)(S.start_ms / 1000));
}

static bool csv_open(void)
{
    if (csv_f && S.csv[0]) return true;
    if (csv_f) { fclose(csv_f); csv_f = NULL; }   /* a new session began before the last one's file closed */
    if (!wd_feed_card()) { S.card = 0; say("no card: the log waits for one"); return false; }
    const char *dir = wd_feed_dir();
    if (!*dir) { S.card = -1; say("cannot make the wardrive folder"); return false; }
    stamp_make();
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.csv", dir, S.stamp);
    /* New for a new session; after a failed write it is carried on. */
    csv_f = fopen(path, S.csv[0] ? "a" : "w");
    if (!csv_f) { S.card = -1; say("cannot write %s", path); return false; }
    fseek(csv_f, 0, SEEK_END);
    if (ftell(csv_f) == 0) {
        char head[320];
        const int n = wd_csv_header(head, sizeof(head), wd_feed_board());
        if (n > 0) fwrite(head, 1, (size_t)n, csv_f);
    }
    wd_lock();
    snprintf(S.csv, sizeof(S.csv), "%s", path);
    S.card = 1;
    wd_unlock();
    return true;
}

static void csv_flush(uint32_t now, bool close)
{
    if (!csv_f) return;
    fflush(csv_f);
#ifdef ESP_PLATFORM
    fsync(fileno(csv_f));
#endif
    csv_flush_ms = now;
    if (close) { fclose(csv_f); csv_f = NULL; }
}

static void csv_append(const char *text, size_t n, uint32_t now)
{
    if (!csv_open()) return;
    if (fwrite(text, 1, n, csv_f) != n) {
        wd_lock(); S.card = -1; say("card write failed"); wd_unlock();
        fclose(csv_f); csv_f = NULL;
        return;
    }
    if (now - csv_flush_ms >= 5000) csv_flush(now, false);
}

/* Rows for every placed network that never got one (no clock or no card
   when it was heard), from where it was strongest. */
static void csv_complete(uint32_t now)
{
    static EXT_RAM_BSS_ATTR char buf[4096];
    if (!csv_open()) return;
    for (int base = 0;; base += 32) {
        size_t n = 0;
        wd_lock();
        const int count = nd;
        for (int i = base; i < count && i < base + 32; i++) {
            wd_dev_t *e = &D[i];
            if ((e->kind != WD_WIFI && e->kind != WD_BLE) || !(e->flags & WD_F_PLACED) || (e->flags & WD_F_ROW)) continue;
            const int w = row(buf + n, sizeof(buf) - n, e, e->rssi, e->lat, e->lon, e->alt, NAN);
            if (w <= 0) continue;
            n += (size_t)w;
            e->flags |= WD_F_ROW;
            S.rows++;
        }
        wd_unlock();
        if (n) fwrite(buf, 1, n, csv_f);
        if (base + 32 >= count) break;
    }
    csv_flush(now, !S.running);
}

static void xml_text(FILE *f, const char *s)
{
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '&') fputs("&amp;", f);
        else if (c == '<') fputs("&lt;", f);
        else if (c == '>') fputs("&gt;", f);
        else if (c == '"') fputs("&quot;", f);
        else fputc(c < 32 ? ' ' : c, f);
    }
}

static void iso(char out[24], long utc)
{
    char t[20];
    if (utc < UTC_MIN || !wd_utc_text(t, (uint32_t)utc)) { out[0] = 0; return; }
    snprintf(out, 24, "%.10sT%.8sZ", t, t + 11);
}

static bool gpx_write(void)
{
    if (!wd_feed_card()) return false;
    const char *dir = wd_feed_dir();
    if (!*dir) return false;
    stamp_make();
    char path[96], when[24];
    snprintf(path, sizeof(path), "%s/%s.gpx", dir, S.stamp);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<gpx version=\"1.1\" creator=\"LakeShark WARDRIVE\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n", f);
    iso(when, S.utc_base + (long)(S.start_ms / 1000));
    fprintf(f, "<metadata><name>%s</name>%s%s%s</metadata>\n", S.stamp, when[0] ? "<time>" : "", when, when[0] ? "</time>" : "");
    /* Cells and drones have no WiGLE row: they are waypoints here. */
    for (int base = 0;; base += 32) {
        wd_lock();
        const int count = nd;
        for (int i = base; i < count && i < base + 32; i++) {
            const wd_dev_t *e = &D[i];
            if ((e->kind != WD_CELL && e->kind != WD_DRONE) || !(e->flags & WD_F_PLACED)) continue;
            fprintf(f, "<wpt lat=\"%.7f\" lon=\"%.7f\">", e->lat, e->lon);
            iso(when, S.utc_base + (long)(e->first_ms / 1000));
            if (when[0]) fprintf(f, "<time>%s</time>", when);
            fputs("<name>", f);
            xml_text(f, e->name[0] ? e->name : e->kind == WD_CELL ? "cell" : "drone");
            fprintf(f, "</name><desc>%s, strongest %d dBm, heard %lu times</desc><type>%s</type></wpt>\n",
                    e->kind == WD_CELL ? "LTE cell" : "drone link", e->rssi, (unsigned long)e->seen,
                    e->kind == WD_CELL ? "cell" : "drone");
        }
        wd_unlock();
        if (base + 32 >= count) break;
    }
    fprintf(f, "<trk><name>%s</name><trkseg>\n", S.stamp);
    for (int base = 0;; base += 64) {
        wd_lock();
        const int count = nt;
        for (int i = base; i < count && i < base + 64; i++) {
            const wd_pt_t *p = &T[i];
            fprintf(f, "<trkpt lat=\"%.7f\" lon=\"%.7f\">", p->lat, p->lon);
            if (isfinite(p->alt)) fprintf(f, "<ele>%.1f</ele>", (double)p->alt);
            iso(when, S.utc_base + (long)(p->ms / 1000));
            if (when[0]) fprintf(f, "<time>%s</time>", when);
            fputs("</trkpt>\n", f);
        }
        wd_unlock();
        if (base + 64 >= count) break;
    }
    fputs("</trkseg></trk>\n</gpx>\n", f);
    const bool ok = !ferror(f);
    fclose(f);
    if (ok) { wd_lock(); snprintf(S.gpx, sizeof(S.gpx), "%s", path); wd_unlock(); }
    return ok;
}

static void do_export(uint32_t now)
{
    csv_complete(now);
    const bool g = gpx_write();
    wd_lock();
    if (!wd_feed_card()) say("no card: nothing written");
    else if (S.card < 0) say("card write failed");
    else say("saved %u rows%s", S.rows, g ? " + GPX track" : ", GPX failed");
    wd_unlock();
}

static void do_locker(void)
{
    char csv[96];
    wd_lock();
    snprintf(csv, sizeof(csv), "%s", S.csv);
    wd_unlock();
    if (!csv[0]) { wd_lock(); say("nothing saved yet to send"); wd_unlock(); return; }
    const bool ok = wd_feed_upload(csv);
    wd_lock();
    say(ok ? "locker: sending the log" : "locker: hub busy, try again");
    gpx_pending = ok && S.gpx[0];
    gpx_sent = false;
    wd_unlock();
}

static void locker_next(void)
{
    if (!gpx_pending) return;
    char text[72];
    bool busy = true;
    if (!wd_feed_hub(text, sizeof(text), &busy) || busy) return;
    char gpx[96];
    wd_lock();
    snprintf(gpx, sizeof(gpx), "%s", S.gpx);
    wd_unlock();
    if (!gpx_sent) {
        gpx_sent = wd_feed_upload(gpx);
        wd_lock(); say(gpx_sent ? "locker: sending the track" : "locker: %s", text); wd_unlock();
        if (!gpx_sent) gpx_pending = false;
        return;
    }
    gpx_pending = false;
    wd_lock(); say("locker: %s", text); wd_unlock();
}

/* ------------------------------------------------------------ the worker */

void wd_step(uint32_t now)
{
    static EXT_RAM_BSS_ATTR wd_obs_t obs[128];
    static EXT_RAM_BSS_ATTR char out[WD_ROWS_BUF];
    wd_fix_t f;
    memset(&f, 0, sizeof(f));
    f.speed_mps = f.course_deg = f.compass_deg = -1;
    wd_lock();
    const bool running = S.running && !S.replay, replay = S.replay;
    wd_unlock();
    if (!replay) wd_feed_fix(&f, now);
    unsigned scans = 0;
    const int n = running ? wd_feed_poll(obs, 128, now, &scans) : 0;
    wd_lock();
    /* A replay owns the fix and the finds; the worker only finishes files. */
    if (!S.replay) wd_fix_note(&f, now);
    if (S.running && !S.replay) {
        for (int i = 0; i < n; i++) wd_note(&obs[i], now);
        if (scans) { S.scans += scans; S.scan_ms = now ? now : 1; }
    }
    const size_t len = rows ? wd_take_rows(out, sizeof(out)) : 0;
    const int r = req;
    req = 0;
    if (r & WD_REQ_EXPORT) s_saving = true;
    const bool stopped = !S.running;
    wd_unlock();
    if (len) csv_append(out, len, now);
    else if (csv_f && now - csv_flush_ms >= 5000) csv_flush(now, false);
    if (r & WD_REQ_EXPORT) { do_export(now); wd_lock(); s_saving = false; wd_unlock(); }
    else if (stopped && csv_f) csv_flush(now, true);
    if (r & WD_REQ_LOCKER) do_locker();
    locker_next();
}

bool wd_busy(void) { return (S.running && !S.replay) || req || gpx_pending || csv_f; }
