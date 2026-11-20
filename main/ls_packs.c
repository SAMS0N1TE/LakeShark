/* See ls_packs.h. One low-priority task on a PSRAM stack: it reads the card
   and asks ls_fetch for files; it never opens a socket or writes flash.
   Packs are installed by the user (ls_packs_install / _install_around); the
   position only picks among the INSTALLED ones. Nothing is removed or fetched
   for a new region by itself; Auto data only refreshes what is installed.
   LANG packs (lang-<code>.bin, an app's translator data) are installed,
   updated and removed the same way but have no place: choose() and
   install_around() pass them by. */
#include "ls_packs.h"
#include "ls_fetch.h"
#include "ls_hub.h"
#include "ls_location.h"
#include "ls_wifi.h"
#include "settings.h"
#include "tui/ls_action.h"
#include "tui/ls_value.h"
#include "ls_textpack.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "mbedtls/sha256.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define PDIR        "/sdcard/lakeshark/packs"
#define CATALOG     PDIR "/catalog.txt"
#define VERIFIED    PDIR "/verified.idx"     /* "id bytes date" of every pack whose sha matched the signed catalog */
#define BASE        "https://data.terminalbay.com/v1/"
#define HERE_MAX    16
#define MARGIN_DEG  0.45f                   /* 50 km */
#define WIDE_W      60.0f                   /* a box wider than this (or WIDE_H tall) counts only when it holds the point */
#define WIDE_H      45.0f
#define MOVED_KM    5.0f
#define RETRY_US    (600 * 1000000LL)
#define CATALOG_US  (24 * 3600 * 1000000LL)
#define FRESH_US    (600 * 1000000LL)       /* an install re-reads a catalog older than this */
#define NEAR_N      (LS_PK_COUNT * LS_PACKS_NEAR)
#define CAT_MAX     640
#define PACK_MAX    (2u << 20)              /* the biggest pack the downloader will take */
#define INST_MAX    64
#define VB_MAX      (48 * 1024)
#define F_INST      1
#define F_UPD       2
#define OP_CATALOG  1
#define OP_INSTALL  2
#define OP_REMOVE   4
#define OP_AROUND   8

typedef struct { char id[32]; uint16_t kinds; } here_t;
typedef struct {
    char     id[32], date[9], idate[9];     /* idate: the data date of the file on the card */
    uint16_t kinds;
    uint8_t  flags;
    uint32_t bytes;
    uint8_t  sha[32];
    float    box[4];                        /* w s e n */
} cat_t;

static EXT_RAM_BSS_ATTR here_t s_here[HERE_MAX];
static int s_nhere;
/* Two catalog lists, like s_near: the task fills the idle one and flips s_cl. */
static EXT_RAM_BSS_ATTR cat_t s_cat[2][CAT_MAX];
static EXT_RAM_BSS_ATTR uint16_t s_ord[2][CAT_MAX];     /* indexes in country, state, id order */
static int s_ncat[2];
static volatile uint8_t s_cl;
static volatile uint16_t s_ninst, s_nupd;
/* Requests (under s_mux) and the task's private copy. */
static EXT_RAM_BSS_ATTR char s_qi[INST_MAX][32], s_qr[INST_MAX][32], s_wi[INST_MAX][32], s_wr[INST_MAX][32];
static uint8_t s_nqi, s_nqr;
static volatile uint8_t s_ops;
static double s_ar_lat, s_ar_lon;
static float s_ar_km;
static uint16_t s_ar_kinds;
/* Two lists: the task fills the idle one and flips s_live, so a reader is
   never under a writer. */
static EXT_RAM_BSS_ATTR ls_pack_sys_t s_near[2][NEAR_N];
static uint8_t s_cnt[2][LS_PK_COUNT];
static volatile uint8_t s_live;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ls_packs_status_t s_st;
static TaskHandle_t s_task;

static const char KIND[LS_PK_COUNT][5] = { "P25", "DMR", "NXDN", "FM", "AIR", "WX", "MAR", "HAM", "BC" };
const char *ls_pack_kind_name(unsigned i) { return i < LS_PK_COUNT ? KIND[i] : "?"; }

static int kind_of(const char *s, size_t n)
{
    for (int i = 0; i < LS_PK_COUNT; i++) if (strlen(KIND[i]) == n && !strncmp(KIND[i], s, n)) return i;
    return -1;
}

/* A catalog kind as its bit; LANG is not a kind of system in a pack. */
static uint16_t kind_bit(const char *s, size_t n)
{
    const int i = kind_of(s, n);
    return i >= 0 ? (uint16_t)(1u << i) : n == 4 && !strncmp(s, "LANG", 4) ? (uint16_t)LS_PK_LANG : 0;
}

/* lang-<code> is a translator language: the app's own bytes, in a .bin. */
static bool is_lang(const char *id) { return !strncmp(id, "lang-", 5); }

/* In place: up to max fields; the last one keeps any further separators. */
static int split(char *s, char sep, char **f, int max)
{
    int n = 0;
    f[n++] = s;
    for (; *s && n < max; s++) if (*s == sep) { *s = 0; f[n++] = s + 1; }
    return n;
}

static void set_state(ls_packs_state_t st, int n, int m)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state = st; s_st.n = (uint8_t)n; s_st.m = (uint8_t)m;
    s_st.here = s_st.have = (uint8_t)s_nhere;
    s_st.installed = s_ninst; s_st.updatable = s_nupd;
    if (st != LS_PACKS_FETCHING) s_st.got = s_st.total = 0;
    portEXIT_CRITICAL(&s_mux);
}

void ls_packs_status(ls_packs_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

ls_packs_state_t ls_packs_status_text(char *out, size_t cap)
{
    ls_packs_status_t t;
    ls_packs_status(&t);
    static const char *const T[] = { "idle", "no fix", "no Wi-Fi", "", "", "failed, will retry", "no packs installed" };
    if (t.state == LS_PACKS_FETCHING) {
        if (t.m) snprintf(out, cap, "fetching %u of %u", t.n, t.m);
        else snprintf(out, cap, "fetching list");
    } else if (t.state == LS_PACKS_READY) snprintf(out, cap, "ready: %u packs here", t.have);
    else snprintf(out, cap, "%s", T[t.state]);
    return t.state;
}

/* --------------------------------------------------------------- catalog */

static const char *ext_of(const char *id) { return is_lang(id) ? ".bin" : ".txt"; }
static void pack_path(const char *id, char *out, size_t cap) { snprintf(out, cap, PDIR "/%s%s", id, ext_of(id)); }

/* A finite decimal number that is the whole field. */
static bool num(const char *s, float *out)
{
    char *e;
    *out = strtof(s, &e);
    return e != s && !*e && isfinite(*out);
}

/* Does the box hold the point, within margin_deg (none for a continent-sized
   box, so a bad or stale catalog entry cannot claim the world)? The point is
   also tried one turn round the globe: boxes never cross the antimeridian. */
static bool box_hit(const float *b, double lat, double lon, float margin_deg)
{
    if (b[2] - b[0] > WIDE_W || b[3] - b[1] > WIDE_H) margin_deg = 0;
    const float my = margin_deg, mx = margin_deg / fmaxf(0.2f, cosf((float)lat * 0.0174533f));
    if (lat < b[1] - my || lat > b[3] + my) return false;
    for (int t = -1; t <= 1; t++) {
        const double x = lon + 360.0 * t;
        if (x >= b[0] - mx && x <= b[2] + mx) return true;
    }
    return false;
}

/* The last "id bytes date" line of the verified index that matches. */
static bool vlookup(const char *buf, const char *id, uint32_t sz, char date[9])
{
    char key[56];
    const int kn = snprintf(key, sizeof(key), "%s %u ", id, (unsigned)sz);
    const char *p = buf, *hit = NULL;
    while ((p = strstr(p, key))) { if (p == buf || p[-1] == '\n') hit = p; p += kn; }
    if (!hit || strlen(hit + kn) < 8) return false;
    memcpy(date, hit + kn, 8); date[8] = 0;
    return true;
}

static void vappend(const char *id, uint32_t bytes, const char *date)
{
    FILE *f = fopen(VERIFIED, "a");
    if (!f) return;
    fprintf(f, "%s %u %s\n", id, (unsigned)bytes, date);
    fclose(f);
}

/* sha256 of a file on the card equals the catalog's. */
static bool sha_ok(const char *path, const uint8_t *want)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM), out[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    bool ok = f && b && !mbedtls_sha256_starts(&c, 0);
    for (size_t n; ok && (n = fread(b, 1, 4096, f)) > 0;) ok = !mbedtls_sha256_update(&c, b, n);
    ok = ok && !mbedtls_sha256_finish(&c, out) && !memcmp(out, want, 32);
    mbedtls_sha256_free(&c);
    if (f) fclose(f);
    heap_caps_free(b);
    return ok;
}

/* "lm-us-fl-2" -> country US, state FL; "air-au-2" -> AU; "mar-all" -> "--";
   "lang-pt" -> LS_PACK_LANG_CC, PT. */
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
    if (strlen(p) < 2) return;
    cc[0] = (char)(p[0] & ~0x20); cc[1] = (char)(p[1] & ~0x20); cc[2] = 0;
    if (!strcmp(cc, "US") && p[2] == '-' && p[3] >= 'a' && p[4] >= 'a') {
        sub[0] = (char)(p[3] & ~0x20); sub[1] = (char)(p[4] & ~0x20); sub[2] = 0;
    }
}

static const cat_t *s_sorting;
static int cmp_ord(const void *a, const void *b)
{
    const cat_t *x = &s_sorting[*(const uint16_t *)a], *y = &s_sorting[*(const uint16_t *)b];
    char cx[3], sx[4], cy[3], sy[4];
    group_of(x->id, cx, sx); group_of(y->id, cy, sy);
    int d = strcmp(cx, cy);
    if (!d) d = strcmp(sx, sy);
    return d ? d : strcmp(x->id, y->id);
}

/* The signed catalog on the card into the idle list, each pack marked
   installed when its file is on the card and verified (by the download, or
   by its sha256 against the catalog for a file copied in by hand); updatable
   when the catalog's date is newer than the file's. Returns the count, -1
   with no good catalog. */
static int cat_load(void)
{
    const int side = !s_cl;
    cat_t *c = s_cat[side];
    char line[200];
    int n = 0, inst = 0, upd = 0;
    FILE *f = ls_fetch_file_signed(CATALOG, NULL) ? fopen(CATALOG, "r") : NULL;
    if (!f) return -1;
    char *vb = heap_caps_calloc(1, VB_MAX + 1, MALLOC_CAP_SPIRAM);
    FILE *vf = vb ? fopen(VERIFIED, "r") : NULL;
    if (vf) { fread(vb, 1, VB_MAX, vf); fclose(vf); }
    /* The index only grows, and only its head is read: past half of that it
       is written again below, one line per installed pack, so a new line is
       always inside what the next load reads. */
    struct stat vst;
    const bool compact = vb && !stat(VERIFIED, &vst) && vst.st_size > VB_MAX / 2;
    bool ours = false;
    while (n < CAT_MAX && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "board=LS-data")) ours = true;
        char *v[7], *b[4];
        if (!ours || strncmp(line, "pack ", 5) || split(line + 5, ' ', v, 7) != 7 || split(v[2], ',', b, 4) != 4) continue;
        cat_t *h = &c[n];
        memset(h, 0, sizeof(*h));
        for (char *k = v[1]; *k;) {
            const size_t len = strcspn(k, ",");
            h->kinds |= kind_bit(k, len);
            k += len + (k[len] == ',');
        }
        char want[48];
        const size_t idn = strspn(v[0], "abcdefghijklmnopqrstuvwxyz0123456789-");
        snprintf(want, sizeof(want), "packs/%s%s", v[0], ext_of(v[0]));    /* the one place a pack may live */
        /* a language is LANG alone and lang-<code>; nothing else may be either */
        bool ok = idn && !v[0][idn] && idn < sizeof(h->id) && strlen(v[4]) == 64 && strlen(v[6]) == 8 && !strcmp(want, v[5]) &&
                  (h->kinds == LS_PK_LANG) == is_lang(v[0]) && (!is_lang(v[0]) || idn > 5);
        for (int i = 0; ok && i < 32; i++) {
            unsigned byte;
            char two[3] = { v[4][i * 2], v[4][i * 2 + 1], 0 };
            ok = sscanf(two, "%2x", &byte) == 1;
            h->sha[i] = (uint8_t)byte;
        }
        h->bytes = (uint32_t)strtoul(v[3], NULL, 10);
        for (int i = 0; i < 4; i++) ok = ok && num(b[i], &h->box[i]);
        /* a box that is not numbers, is inside out or off the globe must not claim a place */
        ok = ok && h->box[0] <= h->box[2] && h->box[1] <= h->box[3] && h->box[1] >= -90 && h->box[3] <= 90 &&
             h->box[0] >= -180 && h->box[2] <= 180;
        if (!ok || !h->kinds || !h->bytes || h->bytes > PACK_MAX) continue;
        strcpy(h->id, v[0]); strcpy(h->date, v[6]);
        char p[80];
        struct stat st;
        pack_path(h->id, p, sizeof(p));
        if (vb && !stat(p, &st)) {
            if (vlookup(vb, h->id, (uint32_t)st.st_size, h->idate)) h->flags = F_INST;
            else if ((uint32_t)st.st_size == h->bytes && sha_ok(p, h->sha)) {
                h->flags = F_INST; strcpy(h->idate, h->date);
                vappend(h->id, h->bytes, h->date);
            }
            if ((h->flags & F_INST) && strcmp(h->idate, h->date) < 0) h->flags |= F_UPD;
            inst += (h->flags & F_INST) != 0; upd += (h->flags & F_UPD) != 0;
        }
        n++;
    }
    fclose(f);
    heap_caps_free(vb);
    FILE *w = compact ? fopen(VERIFIED, "w") : NULL;
    for (int i = 0; w && i < n; i++) {
        char p[80];
        struct stat st;
        pack_path(c[i].id, p, sizeof(p));
        if ((c[i].flags & F_INST) && !stat(p, &st)) fprintf(w, "%s %u %s\n", c[i].id, (unsigned)st.st_size, c[i].idate);
    }
    if (w) fclose(w);
    for (int i = 0; i < n; i++) s_ord[side][i] = (uint16_t)i;
    s_sorting = c;
    qsort(s_ord[side], (size_t)n, sizeof(uint16_t), cmp_ord);
    s_ncat[side] = n;
    s_cl = (uint8_t)side;
    s_ninst = (uint16_t)inst; s_nupd = (uint16_t)upd;
    return n;
}

/* Installed packs whose box holds the point (then those within 50 km), into
   s_here. Position picks among what is installed; it never fetches. */
static int choose(double lat, double lon)
{
    const cat_t *c = s_cat[s_cl];
    const int nc = s_ncat[s_cl];
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nc && n < HERE_MAX; i++) {
            /* a language has no place: installed or not, never "here" */
            if (!(c[i].flags & F_INST) || (c[i].kinds & LS_PK_LANG) || !box_hit(c[i].box, lat, lon, pass ? MARGIN_DEG : 0)) continue;
            bool dup = false;
            for (int j = 0; j < n; j++) dup = dup || !strcmp(s_here[j].id, c[i].id);
            if (dup) continue;
            strcpy(s_here[n].id, c[i].id);
            s_here[n++].kinds = c[i].kinds;
        }
    return n;
}

static int cat_find(const char *id)
{
    const cat_t *c = s_cat[s_cl];
    for (int i = 0; i < s_ncat[s_cl]; i++) if (!strcmp(c[i].id, id)) return i;
    return -1;
}

int ls_packs_catalog_regions(ls_pack_region_t *out, int skip, int cap)
{
    const int side = s_cl, n = s_ncat[side];
    if (!out) return n;
    int k = 0;
    for (int j = skip < 0 ? 0 : skip; j < n && k < cap; j++, k++) {
        const cat_t *c = &s_cat[side][s_ord[side][j]];
        ls_pack_region_t *r = &out[k];
        memset(r, 0, sizeof(*r));
        strcpy(r->id, c->id);
        group_of(c->id, r->cc, r->sub);
        r->kinds = c->kinds; r->bytes = c->bytes;
        memcpy(r->box, c->box, sizeof(r->box));
        r->installed = (c->flags & F_INST) != 0; r->updatable = (c->flags & F_UPD) != 0;
    }
    return k;
}

static int queue_ids(char (*q)[32], uint8_t *n, const char *const *ids, int cnt, uint8_t op)
{
    int k = 0;
    if (!s_task || !ids) return 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < cnt && *n < INST_MAX; i++, k++) { strncpy(q[*n], ids[i], 31); q[(*n)++][31] = 0; }
    if (k) s_ops |= op;
    portEXIT_CRITICAL(&s_mux);
    if (k) xTaskNotifyGive(s_task);
    return k;
}

int ls_packs_install(const char *const *ids, int n) { return queue_ids(s_qi, &s_nqi, ids, n, OP_INSTALL); }
int ls_packs_remove(const char *const *ids, int n) { return queue_ids(s_qr, &s_nqr, ids, n, OP_REMOVE); }

bool ls_packs_install_around(double lat, double lon, float km, uint16_t kinds)
{
    if (!s_task) return false;
    portENTER_CRITICAL(&s_mux);
    s_ar_lat = lat; s_ar_lon = lon; s_ar_km = km; s_ar_kinds = kinds ? kinds : LS_PK_ALL;
    s_ops |= OP_AROUND;
    portEXIT_CRITICAL(&s_mux);
    xTaskNotifyGive(s_task);
    return true;
}

bool ls_packs_refresh_catalog(void)
{
    if (!s_task) return false;
    portENTER_CRITICAL(&s_mux);
    s_ops |= OP_CATALOG;
    portEXIT_CRITICAL(&s_mux);
    xTaskNotifyGive(s_task);
    return true;
}

/* ----------------------------------------------------------------- packs */

/* Every S record of the packs for here that passes kinds and range. */
void ls_packs_scan(uint16_t kinds, float within_km, ls_packs_scan_fn fn, void *ctx)
{
    double lat, lon;
    if (!ls_location_here(&lat, &lon)) return;
    ls_pack_sys_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    char *line = heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    for (int p = 0; s && line && p < s_nhere; p++) {
        const here_t *h = &s_here[p];
        char path[80];
        if (!(h->kinds & kinds)) continue;
        pack_path(h->id, path, sizeof(path));
        FILE *f = fopen(path, "r");
        if (!f) continue;
        for (uint32_t off = 0, len; fgets(line, 256, f); off += len) {
            len = (uint32_t)strlen(line);
            char *v[8] = { 0 };
            if (line[0] != 'S' || line[1] != '|') continue;
            line[strcspn(line, "\r\n")] = 0;
            if (split(line, '|', v, 8) < 7) continue;
            const int k = kind_of(v[2], strlen(v[2]));
            if (k < 0 || !(kinds & (1u << k))) continue;
            if (!num(v[4], &s->lat) || !num(v[5], &s->lon) || fabsf(s->lat) > 90 || fabsf(s->lon) > 180) continue;
            s->dist_km = ls_location_km(lat, lon, s->lat, s->lon);
            if (!(s->dist_km <= within_km)) continue;
            if (!num(v[6], &s->radius_km)) s->radius_km = 0;
            s->kind = (uint8_t)k; s->offset = off;
            snprintf(s->id, sizeof(s->id), "%s", v[1]);
            snprintf(s->name, sizeof(s->name), "%s", v[3]);
            snprintf(s->extra, sizeof(s->extra), "%s", v[7] ? v[7] : "");
            strcpy(s->pack, h->id);
            if (!fn(s, ctx)) { p = s_nhere; break; }
        }
        fclose(f);
    }
    heap_caps_free(s);
    heap_caps_free(line);
}

static bool near_keep(const ls_pack_sys_t *s, void *ctx)
{
    const int side = *(int *)ctx;
    ls_pack_sys_t *row = &s_near[side][s->kind * LS_PACKS_NEAR];
    int n = s_cnt[side][s->kind], i = n;
    while (i > 0 && row[i - 1].dist_km > s->dist_km) i--;
    if (i >= LS_PACKS_NEAR) return true;
    if (n == LS_PACKS_NEAR) n--;
    memmove(&row[i + 1], &row[i], (size_t)(n - i) * sizeof(*row));
    row[i] = *s;
    s_cnt[side][s->kind] = (uint8_t)(n + 1);
    return true;
}

static void near_build(void)
{
    int side = !s_live;
    memset(s_cnt[side], 0, sizeof(s_cnt[side]));
    ls_packs_scan(LS_PK_ALL, 1e9f, near_keep, &side);
    s_live = (uint8_t)side;
}

int ls_packs_nearest(uint16_t kinds, float within_km, ls_pack_sys_t *out, int cap)
{
    double lat, lon;
    int n = 0;
    if (!ls_location_here(&lat, &lon) || cap <= 0) return 0;
    const int side = s_live;
    for (int k = 0; k < LS_PK_COUNT; k++) {
        if (!(kinds & (1u << k))) continue;
        for (int j = 0; j < s_cnt[side][k]; j++) {
            const ls_pack_sys_t *s = &s_near[side][k * LS_PACKS_NEAR + j];
            const float d = ls_location_km(lat, lon, s->lat, s->lon);
            int i = n;
            if (d > within_km) continue;
            while (i > 0 && out[i - 1].dist_km > d) i--;
            if (i >= cap) continue;
            if (n == cap) n--;
            memmove(&out[i + 1], &out[i], (size_t)(n - i) * sizeof(*out));
            out[i] = *s;
            out[i].dist_km = d;
            n++;
        }
    }
    return n;
}

int ls_packs_freqs(const ls_pack_sys_t *sys, ls_pack_freq_t *out, int cap)
{
    char path[80], line[256];
    int n = 0;
    pack_path(sys->pack, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    /* First from the system's own line, where its F lines follow; if that
       finds none (the hint is from an older file), the whole pack. */
    for (int pass = 0; pass < 2 && !n; pass++) {
        int seen = 0;
        fseek(f, pass ? 0 : (long)sys->offset, SEEK_SET);
        while (n < cap && fgets(line, sizeof(line), f)) {
            char *v[6] = { 0 };
            if (line[0] == 'S' && !pass && seen++) break;
            if (line[0] != 'F' || line[1] != '|') continue;
            line[strcspn(line, "\r\n")] = 0;
            if (split(line, '|', v, 6) < 5 || strcmp(v[1], sys->id)) continue;
            ls_pack_freq_t *q = &out[n];
            char *e;
            const uint64_t hz = strtoull(v[2], &e, 10);
            if (!hz || *e || hz > 12000000000ull) continue;     /* not a frequency */
            q->hz = hz; n++;
            snprintf(q->role, sizeof(q->role), "%s", v[3]);
            snprintf(q->mode, sizeof(q->mode), "%s", v[4]);
            snprintf(q->label, sizeof(q->label), "%s", v[5] ? v[5] : "");
        }
    }
    fclose(f);
    return n;
}

bool ls_pack_extra(const ls_pack_sys_t *sys, const char *key, char *out, size_t cap)
{
    const size_t kn = strlen(key);
    for (const char *p = sys->extra; *p; p += strcspn(p, ";"), p += *p == ';') {
        if (strncmp(p, key, kn) || p[kn] != '=') continue;
        snprintf(out, cap, "%.*s", (int)strcspn(p + kn + 1, ";"), p + kn + 1);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ task */

/* How get()'s job ended, from its done callback (on the fetch task): the job's
   slot may be taken by another download before the next look at its info. */
static volatile ls_fetch_result_t s_got;
static volatile bool s_got_done;
static void got(int id, ls_fetch_result_t r, void *ctx)
{
    (void)id; (void)ctx;
    s_got = r;
    __atomic_store_n(&s_got_done, true, __ATOMIC_RELEASE);
}

static ls_fetch_result_t get(const char *url, const char *dest, uint32_t bytes, const uint8_t *sha, uint8_t flags)
{
    const ls_fetch_req_t q = { .url = url, .dest = dest, .bytes = bytes, .sha256 = sha,
                               .flags = flags, .max_bytes = PACK_MAX, .timeout_s = 180, .done = got };
    __atomic_store_n(&s_got_done, false, __ATOMIC_RELEASE);
    const int id = ls_fetch_start(&q);
    if (!id) return LS_FETCH_BUSY;
    while (!__atomic_load_n(&s_got_done, __ATOMIC_ACQUIRE)) {
        ls_fetch_info_t i;
        if (ls_fetch_info(id, &i)) {
            portENTER_CRITICAL(&s_mux);
            s_st.got = i.got; s_st.total = i.total;
            portEXIT_CRITICAL(&s_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return s_got;
}

static bool id_ok(const char *id)
{
    const size_t n = strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-");
    return n && !id[n] && n < 32;
}

/* How many pack files the card holds: a change means someone copied packs in or out. */
static int count_files(void)
{
    DIR *d = opendir(PDIR);
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        const size_t len = strlen(e->d_name);
        n += len > 4 && (!strcmp(e->d_name + len - 4, ".txt") || !strcmp(e->d_name + len - 4, ".bin")) &&
             strcmp(e->d_name, "catalog.txt");
    }
    if (d) closedir(d);
    return n;
}

static bool wifi_up(void)
{
    bool online = ls_fetch_online();
    if (!online && ls_wifi_sta_ensure() != ESP_ERR_NOT_FOUND)
        for (int i = 0; i < 60 && !(online = ls_fetch_online()); i++) vTaskDelay(pdMS_TO_TICKS(250));
    return online;
}

static bool grab(int idx, uint8_t bg)
{
    cat_t *h = &s_cat[s_cl][idx];
    char url[160], dest[80];
    snprintf(url, sizeof(url), BASE "packs/%s%s", h->id, ext_of(h->id));
    pack_path(h->id, dest, sizeof(dest));
    if (get(url, dest, h->bytes, h->sha, LS_FETCH_RESUME | bg) != LS_FETCH_OK) return false;
    vappend(h->id, h->bytes, h->date);
    strcpy(h->idate, h->date);
    h->flags = F_INST;
    return true;
}

static void recount(void)
{
    int inst = 0, upd = 0;
    const cat_t *c = s_cat[s_cl];
    for (int i = 0; i < s_ncat[s_cl]; i++) { inst += (c[i].flags & F_INST) != 0; upd += (c[i].flags & F_UPD) != 0; }
    s_ninst = (uint16_t)inst; s_nupd = (uint16_t)upd;
}

static EXT_RAM_BSS_ATTR uint16_t s_job[CAT_MAX];

static void task(void *arg)
{
    (void)arg;
    double at_lat = 0, at_lon = 0;
    bool placed = false, rescan = true;
    int64_t retry_us = 0, catalog_us = 0;
    int nfiles = -1;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
        int nwi, nwr;
        double alat, alon;
        float akm;
        uint16_t akinds;
        uint8_t ops;
        portENTER_CRITICAL(&s_mux);
        ops = s_ops; s_ops = 0;
        nwi = s_nqi; nwr = s_nqr; s_nqi = s_nqr = 0;
        memcpy(s_wi, s_qi, (size_t)nwi * 32); memcpy(s_wr, s_qr, (size_t)nwr * 32);
        alat = s_ar_lat; alon = s_ar_lon; akm = s_ar_km; akinds = s_ar_kinds;
        portEXIT_CRITICAL(&s_mux);
        for (int i = 0; i < nwr; i++) {
            char p[80];
            if (!id_ok(s_wr[i])) continue;
            pack_path(s_wr[i], p, sizeof(p));
            remove(p);
            rescan = true;
        }
        const int nf = count_files();
        if (nf != nfiles) { nfiles = nf; rescan = true; }
        int64_t now = esp_timer_get_time();
        double lat, lon;
        const bool pos = ls_location_here(&lat, &lon);
        const bool moved = pos && (!placed || ls_location_km(at_lat, at_lon, lat, lon) > MOVED_KM);
        const bool want_net = (ops & (OP_CATALOG | OP_INSTALL | OP_AROUND)) != 0;
        /* Auto data: only refresh what is installed, never new regions. */
        const bool auto_net = !want_net && s_ninst && settings_get_auto_data() && now >= retry_us &&
                              (!catalog_us || now - catalog_us > CATALOG_US || s_nupd);
        if (!want_net && !auto_net && !rescan && !moved) continue;
        if ((ops & OP_AROUND) && isnan(alat) && !pos) ls_location_want();
        bool failed = false, changed = false;
        const bool online = want_net ? wifi_up() : (auto_net && ls_fetch_online());
        const uint8_t bg = want_net ? 0 : LS_FETCH_BACKGROUND;
        if (rescan) { cat_load(); rescan = false; changed = true; }
        if (online && (!s_ncat[s_cl] || (ops & OP_CATALOG) || !catalog_us ||
                       now - catalog_us > (want_net ? FRESH_US : CATALOG_US))) {
            set_state(LS_PACKS_FETCHING, 0, 0);
            if (get(BASE "catalog.txt", CATALOG, 0, NULL, LS_FETCH_SIGNED | bg) == LS_FETCH_OK) {
                catalog_us = esp_timer_get_time();
                cat_load();
                changed = true;
            } else failed = true;
        }
        /* The jobs: what the user named, the area around a point, or (Auto data) updates. */
        int nj = 0;
        const cat_t *c = s_cat[s_cl];
        const int nc = s_ncat[s_cl];
        for (int i = 0; online && i < nwi; i++) {
            const int k = id_ok(s_wi[i]) ? cat_find(s_wi[i]) : -1;
            bool dup = false;
            for (int j = 0; j < nj; j++) dup = dup || s_job[j] == k;
            if (k >= 0 && !dup && (!(c[k].flags & F_INST) || (c[k].flags & F_UPD))) s_job[nj++] = (uint16_t)k;
        }
        if (online && (ops & OP_AROUND) && (pos || !isnan(alat))) {
            const double la = isnan(alat) ? lat : alat, lo = isnan(alat) ? lon : alon;
            for (int i = 0; i < nc; i++) {
                bool dup = false;
                for (int j = 0; j < nj; j++) dup = dup || s_job[j] == i;
                if (!dup && (c[i].kinds & akinds & LS_PK_ALL) && !(c[i].kinds & LS_PK_LANG) &&
                    (!(c[i].flags & F_INST) || (c[i].flags & F_UPD)) &&
                    box_hit(c[i].box, la, lo, akm / 111.0f)) s_job[nj++] = (uint16_t)i;
            }
        }
        for (int i = 0; online && auto_net && i < nc; i++)
            if (c[i].flags & F_UPD) s_job[nj++] = (uint16_t)i;
        for (int j = 0; j < nj; j++) {
            set_state(LS_PACKS_FETCHING, j + 1, nj);
            if (grab(s_job[j], bg)) changed = true; else failed = true;
        }
        if (changed) recount();
        if (auto_net) retry_us = failed ? esp_timer_get_time() + RETRY_US : 0;
        if (pos && (moved || changed)) {
            s_nhere = 0;
            const int n = choose(lat, lon);
            s_nhere = n;
            near_build();
            placed = true; at_lat = lat; at_lon = lon;
        }
        ls_packs_state_t st = LS_PACKS_READY;
        if (failed) st = LS_PACKS_FAILED;
        else if (want_net && !online) st = LS_PACKS_NO_WIFI;
        else if (!pos) st = LS_PACKS_NO_FIX;
        else if (!s_ninst) st = LS_PACKS_EMPTY;
        set_state(st, 0, 0);
    }
}

bool ls_packs_get_here(uint16_t kinds) { return ls_packs_install_around(NAN, NAN, 50.0f, kinds); }

/* ------------------------------------------------- values, actions, console */

static bool v_loc_key(ls_val_t *v)
{
    static char text[24];
    ls_location_key(text, sizeof(text));
    v->kind = LS_VAL_TEXT; v->s = text;
    return true;
}
static bool v_packs_state(ls_val_t *v)
{
    static char text[32];
    ls_packs_status_text(text, sizeof(text));
    v->kind = LS_VAL_TEXT; v->s = text;
    return true;
}
static ls_act_status_t a_loc_tap(const ls_args_t *a, ls_val_t *v)
{
    (void)a; (void)v;
    ls_location_key_tap();
    return LS_ACT_OK;
}
static ls_act_status_t a_loc_gps(const ls_args_t *a, ls_val_t *v)
{
    (void)v;
    ls_location_set_enabled(a->v[0].i != 0);
    return LS_ACT_OK;
}
static ls_act_status_t a_packs_here(const ls_args_t *a, ls_val_t *v)
{
    (void)a; (void)v;
    return ls_packs_get_here(LS_PK_ALL) ? LS_ACT_OK : LS_ACT_UNAVAILABLE;
}

void ls_services_start(void)
{
    static bool done;
    if (done) return;
    done = true;
    ls_location_init();
    if (xTaskCreatePinnedToCoreWithCaps(task, "ls_packs", 8192, NULL, 1, &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) s_task = NULL;
    ls_value_publish("loc.key", NULL, v_loc_key);
    ls_value_publish("packs.state", NULL, v_packs_state);
    ls_action_register("loc.tap", "", LS_CAP_POWER, a_loc_tap, "GPS key: turn the receiver on");
    ls_action_register("loc.gps", "i", LS_CAP_POWER, a_loc_gps, "Settings > GPS: 1 on from boot, 0 off");
    ls_action_register("packs.here", "", LS_CAP_STORE, a_packs_here, "get data packs for this position");
}

static bool print_sys(const ls_pack_sys_t *s, void *ctx)
{
    int *left = ctx;
    ls_pack_freq_t *f = heap_caps_malloc(8 * sizeof(*f), MALLOC_CAP_SPIRAM);
    const int n = f ? ls_packs_freqs(s, f, 8) : 0;
    LS_PRINTF(t_ls_packs_sys, "%-4s %5.1f km %-15s %s", KIND[s->kind], (double)s->dist_km, s->id, s->name);
    for (int i = 0; i < n; i++) printf(" %.4f", (double)f[i].hz / 1e6);
    putchar('\n');
    heap_caps_free(f);
    return --*left > 0;
}

static int packs_cmd(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "here")) return !ls_packs_get_here(LS_PK_ALL);
    if (argc >= 2 && !strcmp(argv[1], "around"))
        return !ls_packs_install_around(NAN, NAN, argc > 2 ? strtof(argv[2], NULL) : 50.0f, LS_PK_ALL);
    if (argc >= 2 && !strcmp(argv[1], "catalog")) return !ls_packs_refresh_catalog();
    if (argc >= 3 && !strcmp(argv[1], "install")) { printf("queued %d\n", ls_packs_install((const char *const *)&argv[2], argc - 2)); return 0; }
    if (argc >= 3 && !strcmp(argv[1], "remove")) { printf("queued %d\n", ls_packs_remove((const char *const *)&argv[2], argc - 2)); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        ls_pack_region_t r;
        for (int j = 0; ls_packs_catalog_regions(&r, j, 1) == 1; j++) {
            if (argc > 2 && !strstr(r.id, argv[2])) continue;
            printf("%-2s %-2s %-16s %7u %c%c\n", r.cc, r.sub, r.id, (unsigned)r.bytes, r.installed ? 'I' : '-', r.updatable ? 'U' : '-');
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "auto")) { settings_set_auto_data(!strcmp(argv[2], "on")); return 0; }
    if (argc >= 3 && !strcmp(argv[1], "gps")) { ls_location_set_enabled(!strcmp(argv[2], "on")); return 0; }
    if (argc >= 3 && !strcmp(argv[1], "near")) {
        const int k = kind_of(argv[2], strlen(argv[2]));
        const float km = argc > 3 ? strtof(argv[3], NULL) : 50;
        ls_pack_sys_t *s = heap_caps_malloc(12 * sizeof(*s), MALLOC_CAP_SPIRAM);
        const int n = s ? ls_packs_nearest(k < 0 ? LS_PK_ALL : (uint16_t)(1u << k), km, s, 12) : 0;
        int left = 1;
        for (int i = 0; i < n; i++, left = 1) print_sys(&s[i], &left);
        heap_caps_free(s);
        return 0;
    }
    char text[32], key[24];
    ls_location_t l;
    double lat = 0, lon = 0;
    ls_location_get(&l);
    const int src = ls_location_here(&lat, &lon);
    ls_location_key(key, sizeof(key));
    ls_packs_status_text(text, sizeof(text));
    LS_PRINTF(t_ls_packs_status, "location: %s, %.5f %.5f (source %d), region %s, GPS setting %s\npacks: %s, update installed %s, %u installed, %u updatable\n",
              key, lat, lon, src, l.region[0] ? l.region : "-", settings_get_gps_on() ? "on" : "off",
              text, settings_get_auto_data() ? "on" : "off", (unsigned)s_ninst, (unsigned)s_nupd);
    for (int i = 0; i < s_nhere; i++) printf("  in use: %s\n", s_here[i].id);
    return 0;
}

void ls_services_console_register(void)
{
    static const esp_console_cmd_t c = {
        .command = "packs",
        .help = LS_TXT(con_ls_packs_help, "Data packs and location: bare 'packs' shows position, region and the packs in use; "
                       "'packs list [text]' the catalog (I installed, U update); 'packs install <id>...', 'packs remove <id>...'; "
                       "'packs around [km]' installs what covers here; 'packs catalog' refreshes the list; "
                       "'packs near <KIND|all> [km]' lists the nearest systems with their frequencies; "
                       "'packs auto on|off' updates installed packs on Wi-Fi; 'packs gps on|off'."),
        .hint = LS_TXT(con_ls_packs_hint, "[list|install|remove|around|catalog|near <kind> [km]|auto on|off|gps on|off]"),
        .func = packs_cmd,
    };
    esp_console_cmd_register(&c);
    ls_fetch_console_register();
    ls_hub_console_register();
}
