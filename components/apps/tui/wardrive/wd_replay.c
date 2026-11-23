/* WARDRIVE REPLAY (wd_replay.h). */
#include "wd_replay.h"
#include "wd_core.h"
#include "wd_feed.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_timer.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_EV 49152                /* the most events one replay holds (80 bytes each, PSRAM, sized to the files) */
#define LINE 512

enum { EV_FIX, EV_SEEN };
typedef struct {
    uint32_t t;                     /* seconds from the drive's start */
    uint8_t type, kind, auth, channel;
    int8_t rssi;
    uint8_t addr[6];
    float alt;
    double lat, lon;
    char name[33];
} ev_t;

static ev_t *E;
static int ne, cap_ev;
static bool on, paused, done;
static int speed = 4;
static uint32_t base_ms;            /* session ms at the drive's start */
static double pos_ms;               /* playback position, ms from the start */
static uint32_t last_real;
static uint32_t len_s;
static long utc0;
static unsigned n_pts, n_rows, n_marks;
static char name_now[WD_REPLAY_NAME];
/* the previous fix fed, for speed and course */
static double p_lat, p_lon;
static uint32_t p_t;
static bool p_have;

/* ------------------------------------------------------------ text */

static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

/* n digits at s, or -1: sscanf was most of a line's cost on the board */
static int digits(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

long wd_replay_utc(const char *s)
{
    if (!s || s[4] != '-' || s[7] != '-' || (s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':') return -1;
    const int y = digits(s, 4), mo = digits(s + 5, 2), d = digits(s + 8, 2), h = digits(s + 11, 2),
              mi = digits(s + 14, 2), se = digits(s + 17, 2);
    if (y < 0 || mo < 0 || d < 0 || h < 0 || mi < 0 || se < 0) return -1;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return -1;
    return days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400L + h * 3600L + mi * 60L + se;
}

/* Splits one CSV line in place (quotes, doubled quotes inside them). */
int wd_replay_csv_fields(char *line, char *field[], int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        char *out = p;
        field[n++] = p;
        if (*p == '"') {
            char *r = p + 1;
            for (;;) {
                if (!*r) break;
                if (*r == '"') { if (r[1] == '"') { *out++ = '"'; r += 2; continue; } r++; break; }
                *out++ = *r++;
            }
            while (*r && *r != ',') r++;
            const bool more = *r == ',';
            *out = 0;
            if (!more) break;
            p = r + 1;
        } else {
            while (*p && *p != ',' && *p != '\r' && *p != '\n') p++;
            const bool more = *p == ',';
            *p = 0;
            if (!more) break;
            p++;
        }
    }
    return n;
}

unsigned wd_replay_auth(const char *c)
{
    for (unsigned a = 0; a < WD_AUTH_UNKNOWN; a++)
        if (!strcmp(c, wd_auth_name(a))) return a;
    if (strstr(c, "WPA3")) return strstr(c, "EAP") ? WD_AUTH_WPA3_EAP : strstr(c, "WPA2") ? WD_AUTH_WPA2_WPA3 : WD_AUTH_WPA3;
    if (strstr(c, "WPA2")) return strstr(c, "EAP") ? WD_AUTH_WPA2_EAP : strstr(c, "[WPA-") ? WD_AUTH_WPA_WPA2 : WD_AUTH_WPA2;
    if (strstr(c, "WPA")) return WD_AUTH_WPA;
    if (strstr(c, "WEP")) return WD_AUTH_WEP;
    if (strstr(c, "OWE")) return WD_AUTH_OWE;
    if (strstr(c, "WAPI")) return WD_AUTH_WAPI;
    return strstr(c, "[ESS]") || !*c ? WD_AUTH_OPEN : WD_AUTH_UNKNOWN;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool mac(const char *s, uint8_t a[6])
{
    for (int i = 0; i < 6; i++) {
        const int h = hexv(s[i * 3]), l = hexv(s[i * 3 + 1]);
        if (h < 0 || l < 0 || (i < 5 && s[i * 3 + 2] != ':')) return false;
        a[i] = (uint8_t)(h << 4 | l);
    }
    return true;
}

/* A decimal like -71.1234567: whole part and up to nine places, one
   division at the end (strtod in software double was slow on the board). */
static double dec(const char *s)
{
    bool neg = false;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    int64_t whole = 0, frac = 0, scale = 1;
    while (*s >= '0' && *s <= '9') whole = whole * 10 + (*s++ - '0');
    if (*s == '.')
        for (s++; *s >= '0' && *s <= '9'; s++)
            if (scale < 1000000000LL) { frac = frac * 10 + (*s - '0'); scale *= 10; }
    const double v = (double)whole + (double)frac / (double)scale;
    return neg ? -v : v;
}

/* An attribute's value in a GPX tag: lat="..." */
static bool attr(const char *tag, const char *key, double *out)
{
    char k[12];
    snprintf(k, sizeof(k), "%s=\"", key);
    const char *p = strstr(tag, k);
    if (!p) return false;
    *out = dec(p + strlen(k));
    return isfinite(*out);
}
/* Text between <el> and </el>, copied. */
static bool inner(const char *s, const char *el, char *out, size_t n)
{
    char a[16], b[16];
    snprintf(a, sizeof(a), "<%s>", el);
    snprintf(b, sizeof(b), "</%s>", el);
    const char *p = strstr(s, a), *q;
    if (!p) return false;
    p += strlen(a);
    if (!(q = strstr(p, b))) return false;
    size_t k = (size_t)(q - p);
    if (k >= n) k = n - 1;
    memcpy(out, p, k);
    out[k] = 0;
    return true;
}

/* ------------------------------------------------------------ reading */

/* A drive is read a slice at a time from the frame (wd_replay_tick), never
   in one go: 3.9 MB of log read at once held the screen until the stall
   check stopped the board (2026-11-22). Three runs, each in time order:
   the track, the marks (cells and drones, sorted when read), the rows. The
   feed takes the earliest of the three heads, so nothing is sorted whole. */
enum { R_FIX, R_MARK, R_ROW, RUNS };
static int run_lo[RUNS], run_hi[RUNS], cur[RUNS];
enum { L_IDLE, L_ROOM, L_GPX, L_CSV };
#define ROOM_MS 4000                /* how long an open waits for memory to be given back */
#define MIN_EV 16                   /* the smallest part of a drive worth playing */
static uint32_t room_t0;
static long l_size[2];              /* the GPX's and the CSV's bytes */
static int stage;
static FILE *lf;
static long l_read, l_total;
static char l_dir[96], l_stem[WD_REPLAY_NAME];
static unsigned l_k;                /* track points read (a clock for a track without one) */
static int c_tc;                    /* the track point the rows have reached */
static uint32_t c_last;
#define SLICE_LINES 400             /* lines a frame reads */

static ev_t *add(void) { return ne < cap_ev ? &E[ne++] : NULL; }

static bool gpx_line(const char *line)
{
    const bool pt = strstr(line, "<trkpt ") != NULL, wpt = strstr(line, "<wpt ") != NULL;
    if (!pt && !wpt) return true;
    double lat, lon;
    if (!attr(line, "lat", &lat) || !attr(line, "lon", &lon)) return true;
    ev_t *e = add();
    if (!e) return false;
    memset(e, 0, sizeof(*e));
    e->lat = lat; e->lon = lon; e->alt = NAN;
    char t[40];
    const long u = inner(line, "time", t, sizeof(t)) ? wd_replay_utc(t) : -1;
    e->t = u > 0 ? (uint32_t)u : 0;
    if (inner(line, "ele", t, sizeof(t))) e->alt = strtof(t, NULL);
    if (pt) { e->type = EV_FIX; if (!e->t) e->t = 2 * l_k; l_k++; n_pts++; return true; }
    e->type = EV_SEEN;
    char ty[16] = "", nm[40] = "", desc[80] = "";
    inner(line, "type", ty, sizeof(ty));
    inner(line, "name", nm, sizeof(nm));
    inner(line, "desc", desc, sizeof(desc));
    e->kind = !strcmp(ty, "drone") ? WD_DRONE : WD_CELL;
    snprintf(e->name, sizeof(e->name), "%.32s", nm);
    const char *db = strstr(desc, "strongest ");
    e->rssi = (int8_t)(db ? atoi(db + 10) : -90);
    /* No address in the file: one made from the name and the place. */
    uint32_t h = 2166136261u;
    for (const char *c = nm; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    h ^= (uint32_t)(lat * 1e5) * 2654435761u ^ (uint32_t)(lon * 1e5);
    e->addr[0] = 0x02; e->addr[1] = (uint8_t)e->kind;
    e->addr[2] = (uint8_t)(h >> 24); e->addr[3] = (uint8_t)(h >> 16); e->addr[4] = (uint8_t)(h >> 8); e->addr[5] = (uint8_t)h;
    n_marks++;
    return true;
}

/* WiGLE rows. FirstSeen is when the network was first heard, also on the
   rows written again later (stronger, or 50 m on): a later row takes the
   time of the track point nearest where it was written, going forward. */
static bool csv_line(char *line)
{
    if (!strncmp(line, "WigleWifi", 9) || !strncmp(line, "MAC,", 4)) return true;
    char *fl[12];
    if (wd_replay_csv_fields(line, fl, 12) < 11) return true;
    uint8_t a[6];
    const long first = wd_replay_utc(fl[3]);
    if (!mac(fl[0], a) || first < 0) return true;
    ev_t *e = add();
    if (!e) return false;
    memset(e, 0, sizeof(*e));
    e->type = EV_SEEN;
    memcpy(e->addr, a, 6);
    snprintf(e->name, sizeof(e->name), "%.32s", fl[1]);
    e->kind = !strncmp(fl[10], "BLE", 3) ? WD_BLE : WD_WIFI;
    e->auth = (uint8_t)(e->kind == WD_WIFI ? wd_replay_auth(fl[2]) : WD_AUTH_UNKNOWN);
    e->channel = (uint8_t)atoi(fl[4]);
    e->rssi = (int8_t)atoi(fl[5]);
    e->lat = dec(fl[6]);
    e->lon = dec(fl[7]);
    e->alt = fl[8][0] ? strtof(fl[8], NULL) : NAN;
    uint32_t t = (uint32_t)first;
    const int tn = run_hi[R_FIX];
    if (t < c_last && tn) {
        /* a row written again: the nearest track point from here on (a short look ahead) */
        /* the board writes a row within a few points of where it is: a short
           look in single precision is plenty (400 in double was the other
           half of the cost) */
        float best = 1e30f;
        int at = c_tc;
        const float k = cosf((float)e->lat * 0.0174533f);
        for (int i = c_tc; i < tn && i < c_tc + 48; i++) {
            const ev_t *p = &E[i];
            if (p->t < c_last) continue;
            const float dy = (float)(p->lat - e->lat), dx = (float)(p->lon - e->lon) * k;
            const float d = dx * dx + dy * dy;
            if (d < best) { best = d; at = i; }
        }
        c_tc = at;
        t = E[at].t > c_last ? E[at].t : c_last;
    }
    if (t < c_last) t = c_last;
    e->t = c_last = t;
    n_rows++;
    return true;
}

static int by_time(const void *pa, const void *pb)
{
    const ev_t *a = pa, *b = pb;
    return a->t < b->t ? -1 : a->t > b->t;
}

static bool open_part(const char *ext)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s.%s", l_dir, l_stem, ext);
    lf = fopen(path, "r");
    /* reads in 16 KB pieces, not the stream's default few hundred bytes:
       the card is shared with map downloads and every read waits its turn */
    static EXT_RAM_BSS_ATTR char vbuf[16384];
    if (lf) setvbuf(lf, vbuf, _IOFBF, sizeof(vbuf));
    return lf != NULL;
}

/* The track and marks are in: fixes to the front in file order, the marks
   after them in time order. */
static void gpx_done(void)
{
    int w = 0;
    for (int i = 0; i < ne; i++) if (E[i].type == EV_FIX) { if (w != i) { ev_t t = E[w]; E[w] = E[i]; E[i] = t; } w++; }
    run_lo[R_FIX] = 0; run_hi[R_FIX] = w;
    run_lo[R_MARK] = w; run_hi[R_MARK] = ne;
    qsort(E + w, (size_t)(ne - w), sizeof(ev_t), by_time);
    run_lo[R_ROW] = run_hi[R_ROW] = ne;
    c_tc = 0; c_last = 0;
}

/* ------------------------------------------------------------ the list */

int wd_replay_list(char name[][WD_REPLAY_NAME], char detail[][40], int max)
{
    const char *dir = wd_feed_dir();
    if (!*dir) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        const size_t l = strlen(e->d_name);
        if (l < 5 || l - 4 >= WD_REPLAY_NAME) continue;
        const char *ext = e->d_name + l - 4;
        if (strcmp(ext, ".csv") && strcmp(ext, ".gpx")) continue;
        char stem[WD_REPLAY_NAME];
        snprintf(stem, sizeof(stem), "%.*s", (int)(l - 4), e->d_name);
        int i = 0;
        while (i < n && strcmp(name[i], stem)) i++;
        if (i < n) continue;
        /* newest first: the stems are dates */
        if (n == max && strcmp(stem, name[n - 1]) < 0) continue;
        int at = n < max ? n++ : max - 1;
        while (at > 0 && strcmp(name[at - 1], stem) < 0) { memcpy(name[at], name[at - 1], WD_REPLAY_NAME); at--; }
        snprintf(name[at], WD_REPLAY_NAME, "%s", stem);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char p[128];
        struct stat st;
        long csv = 0, gpx = 0;
        snprintf(p, sizeof(p), "%s/%s.csv", dir, name[i]);
        if (!stat(p, &st)) csv = (long)st.st_size;
        snprintf(p, sizeof(p), "%s/%s.gpx", dir, name[i]);
        if (!stat(p, &st)) gpx = (long)st.st_size;
        snprintf(detail[i], 40, "%s%s  %ld KB", gpx ? "track" : "no track", csv ? " + log" : "", (csv + gpx + 1023) / 1024);
    }
    return n;
}

/* ------------------------------------------------------------ playback */

static bool loading(void) { return stage != L_IDLE; }
static uint32_t ms_at(uint32_t t) { return base_ms + t * 1000u; }

static void feed_one(const ev_t *e)
{
    const uint32_t ms = ms_at(e->t);
    if (e->type == EV_FIX) {
        wd_fix_t f;
        memset(&f, 0, sizeof(f));
        f.fix = true;
        f.lat = e->lat; f.lon = e->lon;
        f.alt_m = e->alt; f.hdop = 1.0f;
        f.speed_mps = f.course_deg = f.compass_deg = -1;
        f.sats = 9;
        f.utc = utc0 ? (uint32_t)(utc0 + (long)e->t) : 0;
        if (p_have) {
            const double dy = (e->lat - p_lat) * 111320.0, dx = (e->lon - p_lon) * 111320.0 * cos(e->lat * M_PI / 180);
            const double d = sqrt(dx * dx + dy * dy);
            /* a fix that jumped (indoors, a bad sample) reads as a speed no car makes: unknown */
            if (e->t > p_t && d / (e->t - p_t) < 70.0) f.speed_mps = (float)(d / (e->t - p_t));
            if (d > 2) { float c = (float)(atan2(dx, dy) * 180 / M_PI); f.course_deg = c < 0 ? c + 360 : c; }
        }
        p_lat = e->lat; p_lon = e->lon; p_t = e->t; p_have = true;
        wd_fix_note(&f, ms);
        return;
    }
    wd_obs_t o;
    memset(&o, 0, sizeof(o));
    o.kind = e->kind; o.auth = e->auth; o.channel = e->channel;
    memcpy(o.addr, e->addr, 6);
    o.rssi = e->rssi;
    o.own_pos = true;               /* the row says where the board was */
    o.lat = e->lat; o.lon = e->lon;
    memcpy(o.name, e->name, sizeof(o.name));
    if (wd_note(&o, ms) >= 0 && e->kind == WD_WIFI) {
        wd_stats_t *s = (wd_stats_t *)wd_stats();
        if (s->scan_ms != ms) { s->scans++; s->scan_ms = ms ? ms : 1; }   /* a scan's worth: the 3D ring goes out */
    }
}

/* The run whose next event is earliest (the track first on a tie: what is
   heard there is placed by it); -1 when all are fed. */
static int next_run(void)
{
    int best = -1;
    for (int r = 0; r < RUNS; r++)
        if (cur[r] < run_hi[r] && (best < 0 || E[cur[r]].t < E[cur[best]].t)) best = r;
    return best;
}

/* Feeds every event up to `upto` ms from the start. */
static void feed(double upto)
{
    wd_lock();
    for (int r; (r = next_run()) >= 0 && (double)E[cur[r]].t * 1000.0 <= upto;) feed_one(&E[cur[r]++]);
    wd_unlock();
    done = next_run() < 0;
}

static bool restart_session(void)
{
    wd_lock();
    const bool ok = wd_begin_replay(base_ms);
    wd_unlock();
    for (int r = 0; r < RUNS; r++) cur[r] = run_lo[r];
    p_have = false;
    done = false;
    return ok;
}

static char l_why[64];

static void load_fail(const char *why)
{
    snprintf(l_why, sizeof(l_why), "%s", why);
    if (lf) { fclose(lf); lf = NULL; }
    stage = L_IDLE;
    if (E) { wd_feed_free(E); E = NULL; }
    ne = 0;
}

/* Everything read: times from the start, the session begun, playing. */
static void load_finish(uint32_t real)
{
    /* No track: the rows' places are the route. */
    if (run_hi[R_FIX] == 0) {
        const int lo = run_lo[R_ROW], hi = run_hi[R_ROW];
        run_lo[R_FIX] = ne;
        for (int i = lo; i < hi; i++) {
            if (E[i].kind > WD_BLE) continue;
            ev_t *f = add();
            if (!f) break;
            *f = E[i];
            f->type = EV_FIX;
            f->name[0] = 0;
        }
        run_hi[R_FIX] = ne;
    }
    if (!ne) { load_fail("nothing to replay in that drive"); return; }
    uint32_t t0 = UINT32_MAX, t1 = 0;
    for (int r = 0; r < RUNS; r++)
        if (run_hi[r] > run_lo[r]) {
            if (E[run_lo[r]].t < t0) t0 = E[run_lo[r]].t;
            if (E[run_hi[r] - 1].t > t1) t1 = E[run_hi[r] - 1].t;
        }
    utc0 = t0 > 1577836800u ? (long)t0 : 0;
    for (int i = 0; i < ne; i++) E[i].t -= t0;
    len_s = t1 - t0;
    base_ms = real;
    pos_ms = 0;
    last_real = real;
    paused = false;
    speed = 4;                      /* fast enough to see a drive move, slow enough to follow it */
    stage = L_IDLE;
    if (!restart_session()) { load_fail(wd_stats()->note[0] ? wd_stats()->note : "the last drive is still saving"); return; }
    on = true;
    feed(0);
}

/* One slice of reading. */
static uint32_t l_t0, l_slices;
static int64_t now_us(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return 0;
#endif
}

/* One slice of reading: lines until 30 ms have gone (the frame stays
   responsive) or a cap, whichever first. */
static void load_step(uint32_t real)
{
    static char line[LINE];
    if (!l_slices++) l_t0 = real;
    const int64_t until = now_us() + 30000;
    for (int n = 0; n < SLICE_LINES * 8 && (n < 16 || now_us() < until); n++) {
        if (!lf || !fgets(line, sizeof(line), lf)) {
            if (lf) { fclose(lf); lf = NULL; }
            if (stage == L_GPX) {
                gpx_done();
                stage = L_CSV;
                if (open_part("csv")) continue;
            }
            run_hi[R_ROW] = ne;
            printf("replay: read %ld bytes, %d events in %u ms over %u frames\n", l_read, ne, (unsigned)(real - l_t0), (unsigned)l_slices);
            l_slices = 0;
            load_finish(real);
            return;
        }
        l_read += (long)strlen(line);
        const bool room = stage == L_GPX ? gpx_line(line) : csv_line(line);
        if (stage == L_CSV) run_hi[R_ROW] = ne;
        if (!room) {                    /* full: play what was read */
            fclose(lf); lf = NULL;
            if (stage == L_GPX) gpx_done();
            run_hi[R_ROW] = ne;
            load_finish(real);
            return;
        }
    }
}

/* The events' memory is in hand: start reading the files. */
static bool begin_reading(void)
{
    ne = 0; n_pts = n_rows = n_marks = 0; l_k = 0;
    for (int r = 0; r < RUNS; r++) run_lo[r] = run_hi[r] = cur[r] = 0;
    l_read = 0; l_total = l_size[0] + l_size[1];
    l_why[0] = 0;
    if (l_size[0] && open_part("gpx")) stage = L_GPX;
    else { gpx_done(); stage = L_CSV; if (!open_part("csv")) { load_fail("cannot read the log"); return false; } }
    return true;
}

bool wd_replay_open(const char *stem, uint32_t real, char *why, size_t n)
{
    if (wd_stats()->running && !wd_stats()->replay) { snprintf(why, n, "STOP the drive first"); return false; }
    wd_replay_close();
    const char *dir = wd_feed_dir();
    if (!*dir) { snprintf(why, n, "no card"); return false; }
    snprintf(l_dir, sizeof(l_dir), "%s", dir);
    snprintf(l_stem, sizeof(l_stem), "%s", stem);
    snprintf(name_now, sizeof(name_now), "%s", stem);
    /* The files' sizes bound the events (no line is under 60 bytes): memory
       for that many, no more. Without a track the rows make the route too. */
    long size[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        char path[160];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s.%s", dir, stem, k ? "csv" : "gpx");
        if (!stat(path, &st)) size[k] = (long)st.st_size;
    }
    if (!size[0] && !size[1]) { snprintf(why, n, "nothing to replay in %s", stem); return false; }
    long lines = (size[0] + size[1]) / 60 + 16;
    if (!size[0]) lines *= 2;
    cap_ev = (int)(lines < MAX_EV ? lines : MAX_EV);
    l_size[0] = size[0]; l_size[1] = size[1];
    l_why[0] = 0;
    E = wd_feed_alloc(sizeof(ev_t) * (size_t)cap_ev);
    if (!E) {
        /* The 3D view and the map give theirs back on their own time (the
           map's worker frees its cells a moment later): wait for it. */
        stage = L_ROOM;
        room_t0 = real;
        l_read = 0; l_total = size[0] + size[1];
        return true;
    }
    if (!begin_reading()) { snprintf(why, n, "%s", l_why); return false; }
    return true;
}

/* Waiting for memory: the whole bound once it is there, and once the wait is
   over the part of the drive that fits (the screen says "long drive"). */
static void room_step(uint32_t real)
{
    E = wd_feed_alloc(sizeof(ev_t) * (size_t)cap_ev);
    if (!E && real - room_t0 >= ROOM_MS)
        while (!E && cap_ev / 2 >= MIN_EV) {
            cap_ev /= 2;
            E = wd_feed_alloc(sizeof(ev_t) * (size_t)cap_ev);
        }
    if (E) { (void)begin_reading(); return; }
    if (real - room_t0 >= ROOM_MS) load_fail("not enough memory to replay");
}

void wd_replay_tick(uint32_t real)
{
    if (stage == L_ROOM) { room_step(real); return; }
    if (loading()) { load_step(real); return; }
    if (!on) return;
    const uint32_t dt = real - last_real;
    last_real = real;
    if (paused || done) return;
    pos_ms += (double)(dt > 500 ? 500 : dt) * speed;
    if (pos_ms > len_s * 1000.0) pos_ms = len_s * 1000.0;
    feed(pos_ms);
}

uint32_t wd_replay_now(uint32_t real)
{
    return on ? base_ms + (uint32_t)pos_ms : real;
}

bool wd_replay_on(void) { return on || loading(); }

const char *wd_replay_error(void) { return l_why; }

void wd_replay_close(void)
{
    if (on) {
        wd_lock();
        wd_end(base_ms + (uint32_t)pos_ms);
        wd_stats_t *s = (wd_stats_t *)wd_stats();
        s->replay = false;
        wd_unlock();
    }
    on = false;
    if (lf) { fclose(lf); lf = NULL; }
    stage = L_IDLE;
    if (E) { wd_feed_free(E); E = NULL; }
    ne = 0;
}

void wd_replay_info(wd_replay_info_t *o)
{
    memset(o, 0, sizeof(*o));
    o->on = on; o->paused = paused; o->done = done; o->speed = speed;
    o->loading = loading();
    o->read_pct = l_total ? (int)(l_read * 100 / l_total) : 0;
    o->pos_s = (uint32_t)(pos_ms / 1000); o->len_s = len_s;
    o->points = n_pts; o->rows = n_rows; o->marks = n_marks;
    o->full = ne >= cap_ev;
    snprintf(o->name, sizeof(o->name), "%s", name_now);
}

void wd_replay_pause(uint32_t real)
{
    if (!on) return;
    if (done) { wd_replay_seek(0, real); paused = false; return; }   /* at the end: from the top */
    paused = !paused;
    last_real = real;
}

void wd_replay_speed(int dir, uint32_t real)
{
    (void)real;
    static const int S[] = { 1, 2, 4, 8, 16, 32 };
    int i = 0;
    while (i < 5 && S[i] != speed) i++;
    i += dir > 0 ? 1 : -1;
    speed = S[i < 0 ? 0 : i > 5 ? 5 : i];
}

void wd_replay_seek(float frac, uint32_t real)
{
    if (!on) return;
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    const double to = frac * len_s * 1000.0;
    if (to < pos_ms) restart_session();
    pos_ms = to;
    last_real = real;
    feed(pos_ms);
}

int wd_replay_sightings(const uint8_t addr[6], uint8_t kind, void (*cb)(double lat, double lon, int rssi, void *ctx), void *ctx)
{
    if (!on || !E) return 0;
    int n = 0;
    for (int i = run_lo[R_ROW]; i < run_hi[R_ROW]; i++) {
        const ev_t *e = &E[i];
        if ((double)e->t * 1000.0 > pos_ms) break;     /* rows are in time order */
        if (e->kind != kind || memcmp(e->addr, addr, 6)) continue;
        cb(e->lat, e->lon, e->rssi, ctx);
        n++;
    }
    for (int i = run_lo[R_MARK]; i < run_hi[R_MARK]; i++) {
        const ev_t *e = &E[i];
        if ((double)e->t * 1000.0 > pos_ms || e->kind != kind || memcmp(e->addr, addr, 6)) continue;
        cb(e->lat, e->lon, e->rssi, ctx);
        n++;
    }
    return n;
}

int wd_replay_track(void (*cb)(double lat, double lon, uint32_t s, void *ctx), void *ctx)
{
    if (!on || !E) return 0;
    for (int i = run_lo[R_FIX]; i < run_hi[R_FIX]; i++) cb(E[i].lat, E[i].lon, E[i].t, ctx);
    return run_hi[R_FIX] - run_lo[R_FIX];
}
