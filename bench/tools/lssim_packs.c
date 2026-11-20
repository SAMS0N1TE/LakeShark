/* The data-pack and location services for the simulator: the same API as
   main/ls_packs.c and core/ls_location.c, over the files in
   build/lssim_sdcard/lakeshark/packs (catalog.txt and the pack files, copied
   in by hand as on a card). No signature or hash check, no task, no network:
   enough to draw PACKS and the HERE lists.

     LSSIM_HERE=lat,lon      the position (default central Florida)
     LSSIM_GPS=off|nofix     the GPS key's state (default a 3D fix)
     LSSIM_PACKS_STATE=fetching|nowifi|failed   what ls_packs_status says
     LSSIM_PACKS_SRC=dir     where INSTALL copies a pack from (instant) */
#include "ls_packs.h"
#include "ls_location.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SIM_PDIR "build/lssim_sdcard/lakeshark/packs"
#define SIM_CAT  640
#define SIM_NEAR 160

typedef struct { ls_pack_region_t r; char date[9]; } sim_cat_t;
static sim_cat_t s_cat[SIM_CAT];
static int s_ncat = -1;
static ls_pack_sys_t s_near[SIM_NEAR];
static int s_nnear = -1;
static const char KIND[LS_PK_COUNT][5] = { "P25", "DMR", "NXDN", "FM", "AIR", "WX", "MAR", "HAM", "BC" };
const char *ls_pack_kind_name(unsigned i) { return i < LS_PK_COUNT ? KIND[i] : "?"; }

static int kind_of(const char *s, size_t n)
{
    for (int i = 0; i < LS_PK_COUNT; i++) if (strlen(KIND[i]) == n && !strncmp(KIND[i], s, n)) return i;
    return -1;
}

/* As the board: lang-<code> is a translator language, LANG, in a .bin. */
static bool is_lang(const char *id) { return !strncmp(id, "lang-", 5); }
static const char *ext_of(const char *id) { return is_lang(id) ? ".bin" : ".txt"; }

/* ------------------------------------------------------------ location -- */

static void here_get(double *lat, double *lon)
{
    const char *h = getenv("LSSIM_HERE");
    *lat = 28.54; *lon = -81.38;
    if (h) sscanf(h, "%lf,%lf", lat, lon);
}

static ls_loc_state_t sim_gps(void)
{
    const char *g = getenv("LSSIM_GPS");
    return !g ? LS_LOC_FIX : !strcmp(g, "off") ? LS_LOC_OFF : !strcmp(g, "nofix") ? LS_LOC_NO_FIX : LS_LOC_FIX;
}

void ls_location_get(ls_location_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = sim_gps();
    out->fix = out->have = out->state == LS_LOC_FIX;
    here_get(&out->lat, &out->lon);
    out->sats = 9; out->dim = 3; out->quality = 1;
    snprintf(out->region, sizeof(out->region), "US-FL");
}
ls_loc_src_t ls_location_here(double *lat, double *lon)
{
    here_get(lat, lon);
    return sim_gps() == LS_LOC_FIX ? LS_LOC_SRC_LIVE : LS_LOC_SRC_LAST;
}
ls_loc_state_t ls_location_key(char *out, size_t cap)
{
    const ls_loc_state_t st = sim_gps();
    snprintf(out, cap, "%s", st == LS_LOC_FIX ? "GPS 3D 9 sats" : st == LS_LOC_OFF ? "GPS OFF" : "NO FIX");
    return st;
}
void ls_location_key_tap(void) { }
void ls_location_want(void) { }
float ls_location_km(double lat1, double lon1, double lat2, double lon2)
{
    const double k = 0.0174532925, x = (lon2 - lon1) * k * cos((lat1 + lat2) * 0.5 * k), y = (lat2 - lat1) * k;
    return (float)(6371.0 * sqrt(x * x + y * y));
}

/* ------------------------------------------------------------- catalog -- */

static void group_of(const char *id, char cc[3], char sub[4])
{
    cc[0] = sub[0] = 0;
    const char *p = strchr(id, '-');
    if (!p) return;
    p++;
    if (is_lang(id)) {
        strcpy(cc, LS_PACK_LANG_CC);
        for (int i = 0; i < 3 && p[i]; i++) { sub[i] = p[i] >= 'a' && p[i] <= 'z' ? (char)(p[i] - 32) : p[i]; sub[i + 1] = 0; }
        return;
    }
    if (!strcmp(p, "all")) { strcpy(cc, "--"); return; }
    cc[0] = (char)(p[0] & ~0x20); cc[1] = (char)(p[1] & ~0x20); cc[2] = 0;
    if (!strcmp(cc, "US") && p[2] == '-') { sub[0] = (char)(p[3] & ~0x20); sub[1] = (char)(p[4] & ~0x20); sub[2] = 0; }
}

static int cmp_cat(const void *a, const void *b)
{
    const ls_pack_region_t *x = &((const sim_cat_t *)a)->r, *y = &((const sim_cat_t *)b)->r;
    int d = strcmp(x->cc, y->cc);
    if (!d) d = strcmp(x->sub, y->sub);
    return d ? d : strcmp(x->id, y->id);
}

static void cat_load(void)
{
    s_ncat = 0;
    FILE *f = fopen(SIM_PDIR "/catalog.txt", "r");
    char line[256];
    while (f && s_ncat < SIM_CAT && fgets(line, sizeof(line), f)) {
        char id[40], kinds[64], box[96], sha[80], path[80], date[16];
        unsigned long bytes;
        if (sscanf(line, "pack %39s %63s %95s %lu %79s %79s %15s", id, kinds, box, &bytes, sha, path, date) != 7) continue;
        sim_cat_t *c = &s_cat[s_ncat];
        memset(c, 0, sizeof(*c));
        snprintf(c->r.id, sizeof(c->r.id), "%s", id);
        for (char *k = kinds; *k;) {
            const size_t n = strcspn(k, ",");
            const int i = kind_of(k, n);
            if (i >= 0) c->r.kinds |= (uint16_t)(1u << i);
            else if (n == 4 && !strncmp(k, "LANG", 4)) c->r.kinds |= LS_PK_LANG;
            k += n + (k[n] == ',');
        }
        sscanf(box, "%f,%f,%f,%f", &c->r.box[0], &c->r.box[1], &c->r.box[2], &c->r.box[3]);
        c->r.bytes = (uint32_t)bytes;
        snprintf(c->date, sizeof(c->date), "%s", date);
        group_of(id, c->r.cc, c->r.sub);
        char p[128];
        struct stat st;
        snprintf(p, sizeof(p), SIM_PDIR "/%s%s", id, ext_of(id));
        c->r.installed = !stat(p, &st) && (uint32_t)st.st_size == c->r.bytes;
        s_ncat++;
    }
    if (f) fclose(f);
    qsort(s_cat, (size_t)s_ncat, sizeof(s_cat[0]), cmp_cat);
    s_nnear = -1;
}

int ls_packs_catalog_regions(ls_pack_region_t *out, int skip, int cap)
{
    if (s_ncat < 0) cat_load();
    if (!out) return s_ncat;
    int k = 0;
    for (int j = skip < 0 ? 0 : skip; j < s_ncat && k < cap; j++) out[k++] = s_cat[j].r;
    return k;
}

static void copy_in(const char *id)
{
    const char *src = getenv("LSSIM_PACKS_SRC");
    if (!src) return;
    char a[512], b[256], buf[4096];
    snprintf(a, sizeof(a), "%s/%s%s", src, id, ext_of(id));
    snprintf(b, sizeof(b), SIM_PDIR "/%s%s", id, ext_of(id));
    FILE *x = fopen(a, "rb"), *y = x ? fopen(b, "wb") : NULL;
    for (size_t n; x && y && (n = fread(buf, 1, sizeof(buf), x)) > 0;) fwrite(buf, 1, n, y);
    if (x) fclose(x);
    if (y) fclose(y);
}

int ls_packs_install(const char *const *ids, int n)
{
    for (int i = 0; i < n; i++) copy_in(ids[i]);
    cat_load();
    return n;
}
int ls_packs_remove(const char *const *ids, int n)
{
    for (int i = 0; i < n; i++) {
        char p[128];
        snprintf(p, sizeof(p), SIM_PDIR "/%s%s", ids[i], ext_of(ids[i]));
        remove(p);
    }
    cat_load();
    return n;
}

static bool box_hit(const float *b, double lat, double lon, float m)
{
    if (b[2] - b[0] > 60 || b[3] - b[1] > 45) m = 0;
    return lat >= b[1] - m && lat <= b[3] + m && lon >= b[0] - m && lon <= b[2] + m;
}

bool ls_packs_install_around(double lat, double lon, float km, uint16_t kinds)
{
    if (isnan(lat)) here_get(&lat, &lon);
    if (s_ncat < 0) cat_load();
    for (int i = 0; i < s_ncat; i++)
        if ((s_cat[i].r.kinds & (kinds ? kinds : LS_PK_ALL) & LS_PK_ALL) && box_hit(s_cat[i].r.box, lat, lon, km / 111.0f))
            copy_in(s_cat[i].r.id);
    cat_load();
    return true;
}
bool ls_packs_refresh_catalog(void) { cat_load(); return true; }
bool ls_packs_get_here(uint16_t kinds) { return ls_packs_install_around(NAN, NAN, 50.0f, kinds); }

static int here_packs(void)
{
    double lat, lon;
    int n = 0;
    here_get(&lat, &lon);
    if (s_ncat < 0) cat_load();
    for (int i = 0; i < s_ncat; i++)
        n += s_cat[i].r.installed && !(s_cat[i].r.kinds & LS_PK_LANG) && box_hit(s_cat[i].r.box, lat, lon, 0.45f);
    return n;
}

void ls_packs_status(ls_packs_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_ncat < 0) cat_load();
    for (int i = 0; i < s_ncat; i++) out->installed += s_cat[i].r.installed;
    out->here = out->have = (uint8_t)here_packs();
    out->state = out->installed ? LS_PACKS_READY : LS_PACKS_EMPTY;
    const char *s = getenv("LSSIM_PACKS_STATE");
    if (s && !strcmp(s, "fetching")) { out->state = LS_PACKS_FETCHING; out->n = 2; out->m = 3; out->got = 291840; out->total = 637213; }
    else if (s && !strcmp(s, "nowifi")) out->state = LS_PACKS_NO_WIFI;
    else if (s && !strcmp(s, "failed")) out->state = LS_PACKS_FAILED;
}

ls_packs_state_t ls_packs_status_text(char *out, size_t cap)
{
    ls_packs_status_t t;
    ls_packs_status(&t);
    if (t.state == LS_PACKS_FETCHING) snprintf(out, cap, "fetching %u of %u", t.n, t.m);
    else if (t.state == LS_PACKS_READY) snprintf(out, cap, "ready: %u packs here", t.have);
    else snprintf(out, cap, "%s", t.state == LS_PACKS_NO_WIFI ? "no Wi-Fi" : t.state == LS_PACKS_EMPTY ? "no packs installed" : "failed, will retry");
    return t.state;
}

/* --------------------------------------------------------------- packs -- */

static char *split(char *s, char **f, int max)
{
    int n = 0;
    f[n++] = s;
    for (; *s && n < max; s++) if (*s == '|') { *s = 0; f[n++] = s + 1; }
    for (; n < max; n++) f[n] = NULL;
    return s;
}

void ls_packs_scan(uint16_t kinds, float within_km, ls_packs_scan_fn fn, void *ctx)
{
    double lat, lon;
    here_get(&lat, &lon);
    if (s_ncat < 0) cat_load();
    char line[300];
    for (int p = 0; p < s_ncat; p++) {
        const ls_pack_region_t *r = &s_cat[p].r;
        if (!r->installed || !(r->kinds & kinds & LS_PK_ALL) || !box_hit(r->box, lat, lon, 0.45f)) continue;
        char path[128];
        snprintf(path, sizeof(path), SIM_PDIR "/%s.txt", r->id);
        FILE *f = fopen(path, "r");
        for (uint32_t off = 0, len; f && fgets(line, sizeof(line), f); off += len) {
            len = (uint32_t)strlen(line);
            if (line[0] != 'S' || line[1] != '|') continue;
            line[strcspn(line, "\r\n")] = 0;
            char *v[8];
            split(line, v, 8);
            if (!v[6]) continue;
            const int k = kind_of(v[2], strlen(v[2]));
            if (k < 0 || !(kinds & (1u << k))) continue;
            ls_pack_sys_t s;
            memset(&s, 0, sizeof(s));
            s.lat = strtof(v[4], NULL); s.lon = strtof(v[5], NULL);
            s.dist_km = ls_location_km(lat, lon, s.lat, s.lon);
            if (s.dist_km > within_km) continue;
            s.kind = (uint8_t)k; s.radius_km = strtof(v[6], NULL); s.offset = off;
            snprintf(s.id, sizeof(s.id), "%s", v[1]);
            snprintf(s.name, sizeof(s.name), "%s", v[3]);
            snprintf(s.extra, sizeof(s.extra), "%s", v[7] ? v[7] : "");
            snprintf(s.pack, sizeof(s.pack), "%s", r->id);
            if (!fn(&s, ctx)) { fclose(f); return; }
        }
        if (f) fclose(f);
    }
}

static bool keep(const ls_pack_sys_t *s, void *ctx)
{
    (void)ctx;
    int i = s_nnear;
    while (i > 0 && s_near[i - 1].dist_km > s->dist_km) i--;
    if (i >= SIM_NEAR) return true;
    const int n = s_nnear < SIM_NEAR ? s_nnear : SIM_NEAR - 1;
    memmove(&s_near[i + 1], &s_near[i], (size_t)(n - i) * sizeof(s_near[0]));
    s_near[i] = *s;
    s_nnear = n + 1;
    return true;
}

int ls_packs_nearest(uint16_t kinds, float within_km, ls_pack_sys_t *out, int cap)
{
    /* The board keeps the nearest of each kind; here, the nearest of the
       kinds last asked for. */
    static uint16_t last;
    if (s_nnear < 0 || kinds != last) { s_nnear = 0; last = kinds; ls_packs_scan(kinds, 400.0f, keep, NULL); }
    int n = 0;
    for (int i = 0; i < s_nnear && n < cap; i++)
        if ((kinds & (1u << s_near[i].kind)) && s_near[i].dist_km <= within_km) out[n++] = s_near[i];
    return n;
}

int ls_packs_freqs(const ls_pack_sys_t *sys, ls_pack_freq_t *out, int cap)
{
    char path[128], line[300];
    int n = 0;
    snprintf(path, sizeof(path), SIM_PDIR "/%s.txt", sys->pack);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fseek(f, (long)sys->offset, SEEK_SET);
    bool in = false;
    while (n < cap && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v[6];
        if (line[0] == 'S') { if (in) break; in = true; continue; }
        if (line[0] != 'F') continue;
        split(line, v, 6);
        if (!v[4] || strcmp(v[1], sys->id)) continue;
        ls_pack_freq_t *q = &out[n++];
        memset(q, 0, sizeof(*q));
        q->hz = strtoull(v[2], NULL, 10);
        snprintf(q->role, sizeof(q->role), "%s", v[3]);
        snprintf(q->mode, sizeof(q->mode), "%s", v[4]);
        snprintf(q->label, sizeof(q->label), "%s", v[5] ? v[5] : "");
    }
    fclose(f);
    return n;
}

bool ls_pack_extra(const ls_pack_sys_t *sys, const char *key, char *out, size_t cap)
{
    const size_t kn = strlen(key);
    for (const char *p = sys->extra; *p;) {
        const char *end = strchr(p, ';');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > kn && !strncmp(p, key, kn) && p[kn] == '=') {
            snprintf(out, cap, "%.*s", (int)(len - kn - 1), p + kn + 1);
            return true;
        }
        p += len + (end != NULL);
    }
    return false;
}
