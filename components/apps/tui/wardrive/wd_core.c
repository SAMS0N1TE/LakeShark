/* WARDRIVE's session (wd_core.h). Plain C and stdio: the same file runs on
   the board's worker task and in the simulator's frame. */
#include "wd_core.h"
#include "wd_feed.h"
#include "wd_sig.h"
#include "wd_track.h"
#include "wd_atlas.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#ifdef ESP_PLATFORM
#include <unistd.h>
#endif
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_timer.h"
#include "../ls_seen.h"
#include "../ls_sweep_app.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define M_PER_DEG 111194.9266
#define RELOG_M 50.0
#define BUCKETS 1024u
#define FRESH_MS 10000u
/* The GPS gate. A fix is weak when its HDOP is over WD_HDOP_MAX (about 15 m:
   the CSV's accuracy is HDOP x 5) or it has fewer than 4 satellites; a weak
   fix shows on screen but moves nothing. After the start or a loss, the
   position has to hold for SETTLE good fixes (each within SETTLE_M of the
   last) before it counts: a cold GPS walks off a couple of hundred metres
   in its first minute (WD_20261125_015737: 170 m north across the river at
   HDOP 4.5, then back). A jump faster than JUMP_MPS is never a car. */
#define WD_HDOP_MAX 3.0f
#define SETTLE      3
#define SETTLE_M    25.0f
#define JUMP_MPS    70.0f
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

/* The signature table with the advert's IDs when the feed had them. */
static uint16_t sig_match(const wd_obs_t *o, uint16_t *t)
{
    if (!o->ad_have) return wd_sig_match(o, t);
    const wd_sig_ble_t b = {
        .company = (o->ad_have & 1) ? o->company : 0, .mfg0 = (o->ad_have & 1) ? o->mfg0 : -1,
        .uuid16 = (o->ad_have & 2) ? o->uuid16 : 0, .svc0 = (o->ad_have & 2) ? o->svc0 : -1,
    };
    return wd_sig_match_ble(o, &b, t);
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

const char *wd_kind_name(int k)
{
    static const char *const N[WD_KINDS] = { "WI-FI", "BLUETOOTH", "CELL", "DRONES", "SUB-GHZ", "AIRCRAFT", "LORA" };
    return k >= 0 && k < WD_KINDS ? N[k] : "?";
}

const char *wd_kind_short(int k)
{
    static const char *const N[WD_KINDS] = { "WI-FI", "BT", "CELL", "DRONE", "RF", "AIR", "LORA" };
    return k >= 0 && k < WD_KINDS ? N[k] : "?";
}

const char *wd_kind_tag(int k)
{
    static const char *const N[WD_KINDS] = { "WIFI", "BT", "LTE", "UAS", "RF", "AIR", "LRA" };
    return k >= 0 && k < WD_KINDS ? N[k] : "?";
}

const char *wd_tag_word(uint16_t t)
{
    /* most telling first */
    static const struct { uint16_t bit; const char *w; } W[] = {
        { WD_T_ALPR, "ALPR" }, { WD_T_BODYCAM, "BODYCAM" }, { WD_T_POLICE, "POLICE" }, { WD_T_SKIMMER, "SKIMMER" },
        { WD_T_FOLLOW, "FOLLOWING" }, { WD_T_TRACKER, "TRACKER" }, { WD_T_DRONE, "DRONE" }, { WD_T_WATCH, "WATCHED" },
        { WD_T_CAMERA, "CAMERA" }, { WD_T_WEARABLE, "GLASSES" }, { WD_T_HACKTOOL, "HACKTOOL" }, { WD_T_INFRA, "SENSOR" },
        { WD_T_VEHICLE, "VEHICLE" }, { WD_T_PEER, "PARTY" }, { WD_T_NEWHERE, "NEW HERE" }, { WD_T_REGULAR, "REGULAR" },
    };
    for (size_t i = 0; i < sizeof(W) / sizeof(W[0]); i++) if (t & W[i].bit) return W[i].w;
    return "";
}

void wd_local(double lat, double lon, float *x, float *z)
{
    double dl = lon - S.lon0;
    if (dl > 180) dl -= 360;
    else if (dl < -180) dl += 360;
    *x = (float)(dl * S.m_lon);
    *z = (float)(-(lat - S.lat0) * S.m_lat);
}


/* ------------------------------------------------------------ drives on the card */

static void mkdir_one(const char *p)
{
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0775);
#endif
}

static bool exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

bool wd_drive_path(char *out, size_t cap, const char *stem, const char *ext, bool make)
{
    const char *dir = wd_feed_dir();
    if (!dir || !*dir || !stem || !*stem) { if (cap) out[0] = 0; return false; }
    char folder[112];
    snprintf(folder, sizeof(folder), "%s/%s", dir, stem);
    if (!make) {
        /* an old drive still loose on the card */
        snprintf(out, cap, "%s/%s.%s", dir, stem, ext);
        if (exists(out)) return true;
    } else mkdir_one(folder);
    snprintf(out, cap, "%s/%s.%s", folder, stem, ext);
    return true;
}

/* A folder is a drive when it holds its own <name>.csv or <name>.gpx. */
static bool drive_folder(const char *dir, const char *s)
{
    if (strlen(s) >= WD_STEM || strchr(s, '.') || !strcmp(s, "atlas")) return false;
    char p[160];
    snprintf(p, sizeof(p), "%s/%s/%s.gpx", dir, s, s);
    if (exists(p)) return true;
    snprintf(p, sizeof(p), "%s/%s/%s.csv", dir, s, s);
    return exists(p);
}

int wd_drives(char (*stem)[WD_STEM], int max)
{
    const char *dir = wd_feed_dir();
    if (!dir || !*dir || max <= 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        char s[WD_STEM];
        const size_t l = strlen(e->d_name);
        if (drive_folder(dir, e->d_name)) snprintf(s, sizeof(s), "%s", e->d_name);    /* a drive's folder */
        else if (l > 4 && l - 4 < WD_STEM &&
                 (!strcmp(e->d_name + l - 4, ".csv") || !strcmp(e->d_name + l - 4, ".gpx")))
            snprintf(s, sizeof(s), "%.*s", (int)(l - 4), e->d_name);                  /* an old loose file */
        else continue;
        int i = 0;
        while (i < n && strcmp(stem[i], s)) i++;
        if (i < n) continue;
        /* newest first: the stems are dates */
        if (n == max && strcmp(s, stem[n - 1]) < 0) continue;
        int at = n < max ? n++ : max - 1;
        while (at > 0 && strcmp(stem[at - 1], s) < 0) { memcpy(stem[at], stem[at - 1], WD_STEM); at--; }
        snprintf(stem[at], WD_STEM, "%s", s);
    }
    closedir(d);
    return n;
}

int wd_drives_tidy(void)
{
    const char *dir = wd_feed_dir();
    if (!dir || !*dir) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int moved = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        const size_t l = strlen(e->d_name);
        if (l <= 4 || l - 4 >= WD_STEM) continue;
        const char *ext = e->d_name + l - 4;
        if (strcmp(ext, ".csv") && strcmp(ext, ".gpx")) continue;
        char stem[WD_STEM], from[160], to[160];
        snprintf(stem, sizeof(stem), "%.*s", (int)(l - 4), e->d_name);
        if (S.running && !strcmp(stem, S.stamp)) continue;          /* the drive being written stays put */
        snprintf(from, sizeof(from), "%s/%s", dir, e->d_name);
        wd_drive_path(to, sizeof(to), stem, ext + 1, true);
        struct stat sf, st2;
        if (!stat(to, &st2) && !stat(from, &sf) && sf.st_mtime >= st2.st_mtime) remove(to);   /* the newer copy wins */
        if (!exists(to) && rename(from, to) == 0) moved++;
    }
    closedir(d);
    return moved;
}

/* drive.txt: key=value lines, written when a drive stops. */
static void summary_write(void)
{
    if (!S.stamp[0] || S.replay) return;
    char p[160];
    wd_drive_path(p, sizeof(p), S.stamp, "txt", true);
    char *slash = strrchr(p, '/');
    if (slash) snprintf(slash + 1, sizeof(p) - (size_t)(slash + 1 - p), "drive.txt");
    FILE *f = fopen(p, "w");
    if (!f) return;
    unsigned finds = 0, targets = 0;
    for (int k = 0; k < WD_KINDS; k++) finds += S.total[k];
    for (int i = 0; i < nd; i++) targets += (D[i].tags & (uint16_t)~(WD_T_REGULAR | WD_T_NEWHERE)) != 0;
    const long start = S.utc_base >= UTC_MIN / 2 ? S.utc_base + (long)(S.start_ms / 1000) : 0;
    fprintf(f, "start_utc=%ld\nsecs=%lu\nkm=%.2f\nfinds=%u\nwifi=%u\nble=%u\ntargets=%u\nrows=%u\ndropped=%u\nlat=%.5f\nlon=%.5f\nboard=%s\n",
            start, (unsigned long)((S.stop_ms - S.start_ms) / 1000), (double)S.dist_m / 1000.0, finds,
            S.total[WD_WIFI], S.total[WD_BLE], targets, S.rows, S.fixes_dropped, S.lat0, S.lon0, wd_feed_board());
    fclose(f);
}

/* UTC seconds of a civil date and time (H. Hinnant's days from civil). */
static uint32_t utc_of(unsigned y, unsigned mo, unsigned dd, unsigned h, unsigned mi, unsigned s)
{
    const int yy = (int)y - (mo <= 2);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(((long)era * 146097 + (long)doe - 719468) * 86400 + h * 3600 + mi * 60 + s);
}

static void txt_path(char *p, size_t n, const char *stem, bool make)
{
    wd_drive_path(p, n, stem, "txt", make);
    char *slash = strrchr(p, '/');
    if (slash) snprintf(slash + 1, n - (size_t)(slash + 1 - p), "drive.txt");
}

/* Cheap: drive.txt if there is one, the files' sizes, the date from the
   stem. Reads no log or track (an old drive's summary is built a slice at
   a time by wd_drive_summarize). */
bool wd_drive_info(const char *stem, wd_drive_info_t *o)
{
    memset(o, 0, sizeof(*o));
    char p[160];
    struct stat st;
    if (wd_drive_path(p, sizeof(p), stem, "csv", false) && !stat(p, &st)) { o->log = true; o->bytes += (long)st.st_size; }
    if (wd_drive_path(p, sizeof(p), stem, "gpx", false) && !stat(p, &st)) { o->track = true; o->bytes += (long)st.st_size; }
    txt_path(p, sizeof(p), stem, false);
    FILE *f = fopen(p, "r");
    if (f) {
        char line[96];
        while (fgets(line, sizeof(line), f)) {
            long v; double dv;
            if (sscanf(line, "start_utc=%ld", &v) == 1) o->start_utc = (uint32_t)v;
            else if (sscanf(line, "secs=%ld", &v) == 1) o->secs = (uint32_t)v;
            else if (sscanf(line, "km=%lf", &dv) == 1) o->km = (float)dv;
            else if (sscanf(line, "finds=%ld", &v) == 1) o->finds = (unsigned)v;
            else if (sscanf(line, "targets=%ld", &v) == 1) o->targets = (unsigned)v;
            else if (sscanf(line, "lat=%lf", &dv) == 1) o->lat = dv;
            else if (sscanf(line, "lon=%lf", &dv) == 1) o->lon = dv;
        }
        fclose(f);
        o->summary = true;
    }
    if (!o->start_utc && !strncmp(stem, "WD_", 3)) {
        unsigned y, mo, dd, h, mi, s;
        if (sscanf(stem, "WD_%4u%2u%2u_%2u%2u%2u", &y, &mo, &dd, &h, &mi, &s) == 6) o->start_utc = utc_of(y, mo, dd, h, mi, s);
    }
    return o->track || o->log;
}

/* One old drive's summary, read a slice at a time: its log's distinct
   addresses (a 64 K-bit sketch), then its track's times, length and start;
   kept in drive.txt when done. */
static struct {
    char stem[WD_STEM];
    int phase;                  /* 0 open log, 1 log, 2 open track, 3 track, 4 write */
    FILE *f;
    wd_drive_info_t o;
    double plat, plon;
    uint32_t pt;                /* the last point's time, 0 none */
    bool have;
    int ln;
} SM;
static EXT_RAM_BSS_ATTR uint8_t sm_seen[8192];

int wd_drive_summarize(const char *stem, size_t budget, wd_drive_info_t *out)
{
    if (!stem || !*stem) {
        /* left half way: let go of the file (the card has only a few) */
        if (SM.f) fclose(SM.f);
        memset(&SM, 0, sizeof(SM));
        return -1;
    }
    if (strcmp(SM.stem, stem)) {
        if (SM.f) fclose(SM.f);
        memset(&SM, 0, sizeof(SM));
        snprintf(SM.stem, sizeof(SM.stem), "%s", stem);
    }
    char p[160], line[256];
    size_t used = 0;
    while (used < budget) {
        switch (SM.phase) {
        case 0:
            memset(sm_seen, 0, sizeof(sm_seen));
            SM.f = wd_drive_path(p, sizeof(p), stem, "csv", false) ? fopen(p, "r") : NULL;
            SM.phase = SM.f ? 1 : 2;
            break;
        case 1:
            if (!fgets(line, sizeof(line), SM.f)) { fclose(SM.f); SM.f = NULL; SM.phase = 2; break; }
            used += strlen(line);
            if (++SM.ln <= 2 || strlen(line) < 17) break;
            {
                uint32_t h = 2166136261u;
                for (int i = 0; i < 17; i++) h = (h ^ (uint8_t)line[i]) * 16777619u;
                h = (h ^ h >> 16) & 0xFFFF;
                if (!(sm_seen[h >> 3] & (1u << (h & 7)))) { sm_seen[h >> 3] |= (uint8_t)(1u << (h & 7)); SM.o.finds++; }
            }
            break;
        case 2:
            SM.f = wd_drive_path(p, sizeof(p), stem, "gpx", false) ? fopen(p, "r") : NULL;
            SM.phase = SM.f ? 3 : 4;
            break;
        case 3: {
            if (!fgets(line, sizeof(line), SM.f)) { fclose(SM.f); SM.f = NULL; SM.phase = 4; break; }
            used += strlen(line);
            const char *t = strstr(line, "<trkpt lat=\"");
            double la, lo;
            if (!t || sscanf(t, "<trkpt lat=\"%lf\" lon=\"%lf\"", &la, &lo) != 2) break;
            const char *tm = strstr(line, "<time>");
            unsigned y, mo, dd, h, mi, s;
            uint32_t utc = 0;
            if (tm && sscanf(tm, "<time>%4u-%2u-%2uT%2u:%2u:%2u", &y, &mo, &dd, &h, &mi, &s) == 6) {
                utc = utc_of(y, mo, dd, h, mi, s);
                if (!SM.o.start_utc) SM.o.start_utc = utc;
                if (utc > SM.o.start_utc) SM.o.secs = utc - SM.o.start_utc;
            }
            if (!SM.have) { SM.o.lat = la; SM.o.lon = lo; }
            else {
                const double dx = (lo - SM.plon) * M_PER_DEG * cos(la * M_PI / 180), dz = (la - SM.plat) * M_PER_DEG;
                const double d = sqrt(dx * dx + dz * dz);
                /* a jump no car makes (the live gate's rule): the step is not
                   driven, the point still becomes the next anchor */
                const uint32_t dt = utc && SM.pt && utc > SM.pt ? utc - SM.pt : 1;
                if (d / (double)dt <= JUMP_MPS) SM.o.km += (float)(d / 1000.0);
            }
            SM.plat = la; SM.plon = lo; SM.pt = utc; SM.have = true;
            break;
        }
        default: {
            txt_path(p, sizeof(p), stem, true);
            FILE *f = exists(p) ? NULL : fopen(p, "w");
            if (f) {
                fprintf(f, "start_utc=%lu\nsecs=%lu\nkm=%.2f\nfinds=%u\nlat=%.5f\nlon=%.5f\nfrom=old drive\n",
                        (unsigned long)SM.o.start_utc, (unsigned long)SM.o.secs, (double)SM.o.km, SM.o.finds, SM.o.lat, SM.o.lon);
                fclose(f);
            }
            wd_drive_info(stem, out);           /* sizes, and the summary just kept */
            if (!out->summary) {                /* no card write: hand the figures over anyway */
                out->secs = SM.o.secs; out->km = SM.o.km; out->finds = SM.o.finds;
                out->lat = SM.o.lat; out->lon = SM.o.lon;
                if (SM.o.start_utc) out->start_utc = SM.o.start_utc;
                out->summary = true;
            }
            SM.stem[0] = 0;
            return 1;
        }
        }
    }
    return 0;
}

/* A position from the atlas's access points while the GPS is weak or gone
   (wd_atlas_locate): it moves the board and the track, never places a find
   (a find placed from the atlas would feed the atlas its own guesses). */
static void trail_add(double lat, double lon, float x, float z, float alt, uint32_t now);
static void wifi_fix(double lat, double lon, float err, int used, uint32_t now)
{
    if (!S.origin) return;
    float x, z;
    wd_local(lat, lon, &x, &z);
    S.wifi_fix = true;
    S.wifi_used = (uint8_t)used;
    S.wifi_err_m = err;
    S.wifi_ms = now ? now : 1;
    if (nt) {
        const wd_pt_t *l = &T[nt - 1];
        const float d = hypotf(x - l->x, z - l->z);
        if (now > l->ms && d * 1000.0f / (float)(now - l->ms) > JUMP_MPS) return;
        if (d < (t_step > err * 0.5f ? t_step : err * 0.5f)) { S.x = x; S.z = z; return; }  /* inside its own error: no step */
        S.dist_m += d;
        S.wifi_m += d;
    }
    trail_add(lat, lon, x, z, S.fix.alt_m, now);
    S.x = x; S.z = z;
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
    wd_track_reset();
    if (fix.fix && fix_ms) {
        S.origin = true;
        S.lat0 = fix.lat; S.lon0 = fix.lon;
        S.m_lat = (float)M_PER_DEG;
        S.m_lon = (float)(M_PER_DEG * cos(fix.lat * M_PI / 180));
    }
    wd_atlas_session(true, false, now);
    return true;
}

bool wd_begin_replay(uint32_t now)
{
    bool all[WD_KINDS];
    for (int k = 0; k < WD_KINDS; k++) all[k] = true;
    const wd_fix_t keep = S.fix;
    const uint32_t keep_ms = S.fix_ms;
    S.fix.fix = false;                 /* the drive's first fix sets the origin, not where the board is now */
    S.fix_ms = 0;
    if (!wd_begin(now, all, false)) { S.fix = keep; S.fix_ms = keep_ms; return false; }
    S.replay = true;
    S.utc_base = 0;
    wd_atlas_session(false, false, now);    /* wd_begin opened a live one: say it was a replay */
    wd_atlas_session(true, true, now);
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
    wd_atlas_session(false, S.replay, now);
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
        S.fix_weak = false;
        S.settle = 0;                       /* a loss: the next position settles again */
        if (f->compass_deg >= 0 && S.speed < 1.0f) S.heading = f->compass_deg;
        return;
    }
    /* the gate (see WD_HDOP_MAX) */
    static double c_lat, c_lon;             /* the last good fix seen, settled or not */
    static uint32_t c_ms;
    /* a replay feeds points its drive already kept: only the jump check below applies */
    const bool weak = !S.replay && ((isfinite(f->hdop) && f->hdop > WD_HDOP_MAX) || (f->sats > 0 && f->sats < 4));
    S.fix = *f;                             /* the screen shows whatever the GPS says */
    if (weak) { S.fix_weak = true; S.settle = 0; S.fixes_dropped++; return; }
    if (!S.replay && S.settle < SETTLE) {
        float ax = 0, az = 0;
        if (c_ms && now - c_ms < 10000) {
            const float dl = (float)((f->lat - c_lat) * M_PER_DEG);
            const float dn = (float)((f->lon - c_lon) * M_PER_DEG * cos(f->lat * M_PI / 180));
            ax = dl; az = dn;
        }
        const bool agrees = c_ms && now - c_ms < 10000 && hypotf(ax, az) <= SETTLE_M + (f->speed_mps > 0 ? f->speed_mps * (float)(now - c_ms) / 1000.0f : 0);
        S.settle = agrees ? (uint8_t)(S.settle + 1) : 1;
        c_lat = f->lat; c_lon = f->lon; c_ms = now ? now : 1;
        if (S.settle < SETTLE) { S.fix_weak = true; S.fixes_dropped++; return; }
    }
    c_lat = f->lat; c_lon = f->lon; c_ms = now ? now : 1;
    S.fix_weak = false;
    S.wifi_fix = false;                     /* the GPS is back */
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
                if (now > l->ms && d * 1000.0f / (float)(now - l->ms) > JUMP_MPS) {
                    S.fixes_dropped++;              /* a jump no car makes: a bad fix */
                    return;
                }
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

/* A target goes where the board's other watchers' finds go (ls_seen), so
   DETECTOR's rules, the HOME lamp and the Flipper summary see what WARDRIVE
   matched, as SWEEP's categories: once when found, once more when it is
   found to follow. Live Wi-Fi and Bluetooth only (the queue speaks those). */
static void seen_post(wd_dev_t *e)
{
#ifdef ESP_PLATFORM
    if (S.replay || e->radio == WD_RADIO_PEER || (e->kind != WD_WIFI && e->kind != WD_BLE)) return;
    const uint16_t t = e->tags;
    int cat = -1;
    if (t & (WD_T_HACKTOOL | WD_T_SKIMMER)) cat = SW_ATTACK;
    else if (t & (WD_T_BODYCAM | WD_T_POLICE)) cat = SW_BODYCAM;
    else if (t & WD_T_DRONE) cat = SW_DRONE;
    else if (t & (WD_T_ALPR | WD_T_CAMERA | WD_T_WEARABLE)) cat = SW_CAMERA;
    else if (t & (WD_T_TRACKER | WD_T_FOLLOW)) cat = SW_TRACKER;
    if (cat < 0) return;
    const uint8_t want = (uint8_t)(1 | ((t & WD_T_FOLLOW) ? 2 : 0));
    if ((e->posted & want) == want) return;
    e->posted |= want;
    ls_seen_observe(e->addr, 0, e->kind == WD_WIFI ? LS_SEEN_WIFI : LS_SEEN_BLE, (uint8_t)cat, e->rssi, true, false,
                    (t & WD_T_FOLLOW) ? SW_WITH_YOU : SW_NEW, esp_timer_get_time());
#else
    (void)e;
#endif
}

int wd_note(const wd_obs_t *o, uint32_t now)
{
    if (!S.running || !D || o->kind >= WD_KINDS || !S.src[o->kind]) return -1;
    const bool fresh = S.fix_ms && S.fix.fix && !S.fix_weak && now - S.fix_ms <= FRESH_MS;   /* never place a find at a weak fix */
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
        e->radio = o->radio;
        e->sub = o->sub;
        e->freq_khz = o->freq_khz;
        snprintf(e->name, sizeof(e->name), "%s", o->name);
        uint16_t t = o->tags;
        e->sig = sig_match(o, &t);
        e->tags = t;
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
        d->tags = e->tags;
        r_head = (r_head + 1) % WD_DROPS;
        if (r_count < WD_DROPS) r_count++;
        added = true;
    } else {
        e = &D[i];
        if (now - e->sample_ms < 1000 && o->rssi <= e->last_rssi) return 0;   /* once a second is plenty */
        if (!e->name[0] && o->name[0]) {
            /* a name said late may be the one the table knows */
            snprintf(e->name, sizeof(e->name), "%s", o->name);
            uint16_t t = e->tags | o->tags;
            const uint16_t sg = sig_match(o, &t);
            if (sg && !e->sig) e->sig = sg;
            e->tags = t;
        } else e->tags |= o->tags;
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
    /* WiGLE rows are what this board heard: a party member's report is not one */
    if ((o->kind == WD_WIFI || o->kind == WD_BLE) && o->radio != WD_RADIO_PEER && fresh && S.utc_base && !S.replay) {
        bool want = !(e->flags & WD_F_ROW) || o->rssi >= e->row_rssi + 6;
        if (!want) {
            float ax, az, bx, bz;
            wd_local(e->row_lat, e->row_lon, &ax, &az);
            wd_local(S.fix.lat, S.fix.lon, &bx, &bz);
            want = hypotf(ax - bx, az - bz) >= RELOG_M;
        }
        if (want) row_queue(e, o->rssi);
    }
    wd_track_note(i, e, added, now);
    wd_atlas_note(i, e, added, now);
    seen_post(e);
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
    char path[160];
    wd_drive_path(path, sizeof(path), S.stamp, "csv", true);       /* the drive's own folder */
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
            if ((e->kind != WD_WIFI && e->kind != WD_BLE) || e->radio == WD_RADIO_PEER || !(e->flags & WD_F_PLACED) || (e->flags & WD_F_ROW)) continue;
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
    char path[160], when[24];
    wd_drive_path(path, sizeof(path), S.stamp, "gpx", true);
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
    if (wd_feed_card()) summary_write();       /* drive.txt: what the replay list shows */
    if (!wd_feed_card()) say("no card: nothing written");
    else if (S.card < 0) say("card write failed");
    else say("saved %u rows%s", S.rows, g ? " + GPX track" : ", GPX failed");
    wd_unlock();
}

static void do_locker(void)
{
    char csv[128];
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
    char gpx[128];
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
        /* no GPS worth having: where the known access points say we are */
        if ((S.fix_weak || !S.fix.fix || now - S.fix_ms > FRESH_MS) && scans) {
            double la, lo;
            float err;
            int used;
            if (wd_atlas_locate(obs, n, &la, &lo, &err, &used)) wifi_fix(la, lo, err, used, now);
        }
        if (S.wifi_fix && now - S.wifi_ms > 30000) S.wifi_fix = false;
    }
    if (S.running) wd_track_tick(now);
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
    wd_atlas_step(now);
}

bool wd_busy(void) { return (S.running && !S.replay) || req || gpx_pending || csv_f || wd_atlas_pending(); }   /* the atlas compacts right after a drive */
