/* STORE backend: catalogs, the app table, and one worker for refresh,
   install, update and remove. See ls_store.h. */
#include "ls_store.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "ls_app.h"
#include "ls_cells_json.h"
#include "ls_fetch.h"
#include "ls_lap.h"

#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#define PS_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define PS_FREE(p) heap_caps_free(p)
extern esp_err_t ls_wifi_sta_ensure(void);   /* main/ls_wifi.h */
static SemaphoreHandle_t s_mx, s_wake;
static StaticSemaphore_t s_mx_buf, s_wake_buf;
static TaskHandle_t s_task;
#else
#define PS_ALLOC(n) malloc(n)
#define PS_FREE(p) free(p)
int ls_store_host_installed(const char *id, char *have, int cap);   /* bench shim */
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif
#endif

#define SRC_N 2
static const char *const CAT_URL[SRC_N] = {"https://terminalbay.com/apps/catalog.json",
                                           "https://apps.terminalbay.com/catalog.json"};
static const char *const BASE[SRC_N] = {"https://terminalbay.com/", "https://apps.terminalbay.com/"};
#define KEEP_DIR "/sdcard/lakeshark/store"
#define DL_TMP "/sdcard/apps/.store-dl"     /* ls_fetch writes under /sdcard only */
#define CAT_CAP (256u * 1024u)
#define JOBS 8

enum { J_REFRESH = 1, J_INSTALL, J_REMOVE };
typedef struct { uint8_t op; char id[32]; } job_t;

EXT_RAM_BSS_ATTR static struct {
    ls_store_app_t apps[LS_STORE_MAX];
    int n;
    bool opened, loaded, live, busy, msg_bad;
    ls_store_phase_t phase;
    char job_id[32], msg[96];
    volatile uint32_t got, total;
    job_t q[JOBS];
    int qh, qn;
    ls_store_app_t *stage;      /* refresh only */
    int sn;
} S;

/* ------------------------------------------------------------ locking -- */

static void init(void)
{
#ifdef ESP_PLATFORM
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    if (!s_wake) s_wake = xSemaphoreCreateBinaryStatic(&s_wake_buf);
#endif
}
void ls_store_lock(void)
{
#ifdef ESP_PLATFORM
    init();
    xSemaphoreTake(s_mx, portMAX_DELAY);
#endif
}
void ls_store_unlock(void)
{
#ifdef ESP_PLATFORM
    xSemaphoreGive(s_mx);
#endif
}

static void say(bool bad, const char *fmt, const char *a, const char *b)
{
    snprintf(S.msg, sizeof(S.msg), fmt, a ? a : "", b ? b : "");
    S.msg_bad = bad;
}

/* ------------------------------------------------------------ queries -- */

int ls_store_count(void) { return S.n; }
const ls_store_app_t *ls_store_at(int i) { return i >= 0 && i < S.n ? &S.apps[i] : NULL; }
int ls_store_find(const char *id)
{
    for (int i = 0; id && i < S.n; i++)
        if (!strcmp(S.apps[i].id, id)) return i;
    return -1;
}

int ls_store_ver_cmp(const char *a, const char *b)
{
    if (*a == 'v') a++;
    if (*b == 'v') b++;
    for (int i = 0; i < 4; i++) {
        char *e;
        const long x = isdigit((unsigned char)*a) ? strtol(a, &e, 10) : 0;
        if (isdigit((unsigned char)*a)) a = e;
        const long y = isdigit((unsigned char)*b) ? strtol(b, &e, 10) : 0;
        if (isdigit((unsigned char)*b)) b = e;
        if (x != y) return x < y ? -1 : 1;
        a = *a == '.' ? a + 1 : "";
        b = *b == '.' ? b + 1 : "";
    }
    return 0;
}

const char *ls_store_fw(void)
{
#ifdef ESP_PLATFORM
    return esp_app_get_description()->version;
#else
    const char *v = getenv("LSSIM_FW");
    return v ? v : "2.23.2";
#endif
}

static bool fw_ok(const ls_store_app_t *a)
{
    const char *fw = ls_store_fw();
    if (*fw == 'v') fw++;
    return !a->minfw[0] || !isdigit((unsigned char)*fw) || ls_store_ver_cmp(fw, a->minfw) >= 0;
}

static bool installed(const char *id, char *have, int cap)
{
#ifdef ESP_PLATFORM
    const ls_lap_info_t *l = ls_lap_by_id(id);
    if (!l || !l->installed) return false;
    snprintf(have, cap, "%s", l->manifest.version);
    return true;
#else
    return ls_store_host_installed(id, have, cap) > 0;
#endif
}

ls_store_state_t ls_store_state(int i, char *have, int cap)
{
    have[0] = 0;
    const ls_store_app_t *a = ls_store_at(i);
    if (!a) return LS_ST_NEW;
    if (installed(a->id, have, cap))
        return ls_store_ver_cmp(a->ver, have) <= 0 ? LS_ST_INSTALLED : fw_ok(a) ? LS_ST_UPDATE : LS_ST_NEEDS_FW;
    if (ls_app_by_id(a->id)) return LS_ST_BUILTIN;
    return fw_ok(a) ? LS_ST_NEW : LS_ST_NEEDS_FW;
}

void ls_store_caps_text(uint8_t caps, char *out, int cap)
{
    static const char *const NAME[] = {"NET", "RX", "TX", "BG", "MIC", "HID"};
    if (caps == 0xFF) { snprintf(out, cap, "not listed"); return; }
    int n = 0;
    out[0] = 0;
    for (int b = 0; b < 6; b++)
        if (caps & (1u << b)) n += snprintf(out + n, n < cap ? cap - n : 0, "%s%s", n ? " " : "", NAME[b]);
    if (!n) snprintf(out, cap, "none");
}

static bool net_up(void)
{
    bool on = ls_fetch_online();
#ifdef ESP_PLATFORM
    if (!on && ls_wifi_sta_ensure() != ESP_ERR_NOT_FOUND)
        for (int i = 0; i < 40 && !(on = ls_fetch_online()); i++) vTaskDelay(pdMS_TO_TICKS(250));
#endif
    return on;
}

void ls_store_status(ls_store_status_t *o)
{
    memset(o, 0, sizeof(*o));
    ls_store_lock();
    o->n = S.n;
    char have[16];
    for (int i = 0; i < S.n; i++) o->updates += ls_store_state(i, have, sizeof(have)) == LS_ST_UPDATE;
    o->loaded = S.loaded;
    o->live = S.live;
    o->busy = S.busy || S.qn;
    o->phase = S.phase;
    snprintf(o->job_id, sizeof(o->job_id), "%s", S.job_id);
    o->got = S.got;
    o->total = S.total;
    o->queued = S.qn;
    o->msg_bad = S.msg_bad;
    snprintf(o->msg, sizeof(o->msg), "%s", S.msg);
    ls_store_unlock();
    o->online = ls_fetch_online();
}

/* -------------------------------------------------------------- parse -- */

/* A JSON string, lenient: too long is cut, escapes and anything not ASCII
   become something the cell font can draw. */
static void jstr(const cells_jparser *p, int at, char *out, size_t cap)
{
    size_t n = 0;
    if (at >= 0 && p->t[at].type == '"')
        for (unsigned i = p->t[at].start; i < p->t[at].end && n + 1 < cap; i++) {
            unsigned char c = (unsigned char)p->s[i];
            if (c == '\\' && i + 1 < p->t[at].end) {
                c = (unsigned char)p->s[++i];
                if (c == 'u' && i + 4 < p->t[at].end) {
                    const unsigned v = (unsigned)strtoul((char[5]){p->s[i + 1], p->s[i + 2], p->s[i + 3], p->s[i + 4], 0}, NULL, 16);
                    i += 4;
                    c = v >= 32 && v < 127 ? (unsigned char)v : v == 0x2014 || v == 0x2013 ? '-' : '?';
                } else if (strchr("bfnrt", c)) c = ' ';
            } else if (c >= 0x80) {
                if ((c & 0xC0) == 0x80) continue;   /* the rest of one UTF-8 character */
                c = '?';
            }
            out[n++] = (char)c;
        }
    out[n] = 0;
}
static void field(const cells_jparser *p, int obj, const char *key, char *out, size_t cap)
{
    jstr(p, cells_jget(p, obj, key), out, cap);
}
static double fnum(const cells_jparser *p, int obj, const char *key, double dflt)
{
    double v;
    return cells_jnumber(p, cells_jget(p, obj, key), &v) ? v : dflt;
}

static bool plain(const char *s, const char *extra)
{
    if (!*s || strstr(s, "..")) return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr(extra, *s)) return false;
    return true;
}

static uint8_t group_of(const char *g, uint8_t caps, const char *cat)
{
    static const char *const G[] = {"radio", "field", "system", "user"};
    for (int i = 0; i < 4; i++)
        if (!strcmp(g, G[i])) return (uint8_t)(i + 1);
    if (caps != 0xFF)   /* as ls_lap.c places a card app that does not say */
        return caps & (LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX) ? LS_APP_GROUP_RADIO
             : caps & LAP_CAP_NETWORK ? LS_APP_GROUP_SYSTEM : LS_APP_GROUP_USER;
    return !strcmp(cat, "radio") ? LS_APP_GROUP_RADIO : !strcmp(cat, "maps") ? LS_APP_GROUP_FIELD
         : !strcmp(cat, "tools") ? LS_APP_GROUP_SYSTEM : LS_APP_GROUP_USER;
}

static int stage_find(const char *id)
{
    for (int i = 0; i < S.sn; i++)
        if (!strcmp(S.stage[i].id, id)) return i;
    return -1;
}

static void parse_one(const cells_jparser *p, int o, int src)
{
    ls_store_app_t *a = &S.stage[S.sn];
    char sha[72];
    memset(a, 0, sizeof(*a));
    field(p, o, "id", a->id, sizeof(a->id));
    field(p, o, "file", a->file, sizeof(a->file));
    field(p, o, "sha256", sha, sizeof(sha));
    const double size = fnum(p, o, "size", 0);
    if (!plain(a->id, "_.-") || strlen(a->id) > 31 || !plain(a->file, "_.-/") || a->file[0] == '/' ||
        strlen(sha) != 64 || size < 52 || size > LS_LAP_MAX_BYTES || stage_find(a->id) >= 0)
        return;
    for (int i = 0; i < 32; i++) {
        char h[3] = {sha[i * 2], sha[i * 2 + 1], 0};
        if (!isxdigit((unsigned char)h[0]) || !isxdigit((unsigned char)h[1])) return;
        a->sha[i] = (uint8_t)strtoul(h, NULL, 16);
    }
    a->size = (uint32_t)size;
    a->src = (uint8_t)src;
    field(p, o, "name", a->name, sizeof(a->name));
    if (!a->name[0]) snprintf(a->name, sizeof(a->name), "%.15s", a->id);
    field(p, o, "sub", a->sub, sizeof(a->sub));
    field(p, o, "version", a->ver, sizeof(a->ver));
    field(p, o, "author", a->author, sizeof(a->author));
    field(p, o, "min_firmware", a->minfw, sizeof(a->minfw));
    field(p, o, "category", a->cat, sizeof(a->cat));
    field(p, o, "license", a->license, sizeof(a->license));
    field(p, o, "description", a->desc, sizeof(a->desc));
    const int pub = cells_jget(p, o, "publisher");
    if (pub >= 0) field(p, pub, "login", a->publisher, sizeof(a->publisher));
    a->caps = (uint8_t)fnum(p, o, "caps", 0xFF);
    a->color = (uint8_t)fnum(p, o, "color", 6) & 15;
    char tmp[16];
    field(p, o, "status", tmp, sizeof(tmp));
    a->wip = !strcmp(tmp, "wip");
    field(p, o, "group", tmp, sizeof(tmp));
    a->group = group_of(tmp, a->caps, a->cat);
    int at = cells_jget(p, o, "icon_art");
    if (at >= 0 && p->t[at].type == '[')
        for (unsigned k = 0, e = (unsigned)at + 1; k < p->t[at].count && k < 6; k++, e = p->t[e].next)
            jstr(p, (int)e, a->art[k], sizeof(a->art[k]));
    at = cells_jget(p, o, "icon_px");      /* catalogs from 11-25 on: the tile in colour */
    if (at >= 0 && p->t[at].type == '[' && p->t[at].count == 18) {
        bool ok = true;
        unsigned k = 0;
        for (unsigned e = (unsigned)at + 1; k < 18 && ok; k++, e = p->t[e].next) {
            char row[24];
            jstr(p, (int)e, row, sizeof(row));
            ok = strlen(row) == 20 && strspn(row, ".0123456789abcdef") == 20;
            if (ok) memcpy(a->px[k], row, 20);
        }
        if (!ok) a->px[0][0] = 0;
    }
    at = cells_jget(p, o, "third_party");
    if (at >= 0 && p->t[at].type == '[') {
        size_t n = 0;
        for (unsigned k = 0, e = (unsigned)at + 1; k < p->t[at].count; k++, e = p->t[e].next) {
            char nm[48], lic[24];
            field(p, (int)e, "name", nm, sizeof(nm));
            field(p, (int)e, "license", lic, sizeof(lic));
            nm[strcspn(nm, ",(:")] = 0;    /* the source's name, not its whole note */
            for (size_t l = strlen(nm); l && nm[l - 1] == ' '; l--) nm[l - 1] = 0;
            n += (size_t)snprintf(a->credits + n, n < sizeof(a->credits) ? sizeof(a->credits) - n : 0,
                                  "%s%s (%s)", n ? "; " : "", nm, lic);
        }
    }
    S.sn++;
}

static void parse(char *buf, unsigned len, int src)
{
    const unsigned cap = len / 4 + 64;
    cells_jtoken *tok = PS_ALLOC(cap * sizeof(cells_jtoken));
    if (!tok) return;
    cells_jparser p = {buf, len, 0, tok, 0, cap};
    if (cells_jparse(&p)) {
        const int apps = cells_jget(&p, 0, "apps");
        if (apps >= 0 && tok[apps].type == '[')
            for (unsigned k = 0, e = (unsigned)apps + 1; k < tok[apps].count && S.sn < LS_STORE_MAX;
                 k++, e = tok[e].next)
                if (tok[e].type == '{') parse_one(&p, (int)e, src);
    }
    PS_FREE(tok);
}

/* --------------------------------------------------------------- jobs -- */

static void keep_path(int src, char *out, size_t cap) { snprintf(out, cap, KEEP_DIR "/catalog%d.json", src); }

/* The kept list is replaced whole or not at all: a full card or a power cut must not leave half of it. */
static void keep_write(const char *path, const char *buf, uint32_t len)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(buf, 1, len, f) == len;
    if (f && fclose(f)) ok = false;
    if (ok) { remove(path); ok = !rename(tmp, path); }
    if (!ok) remove(tmp);
}

static void do_refresh(void)
{
    S.phase = LS_SJ_FETCH;
    char *buf = PS_ALLOC(CAT_CAP);
    S.stage = PS_ALLOC(sizeof(ls_store_app_t) * LS_STORE_MAX);
    if (!buf || !S.stage) {
        say(true, "Out of memory for the catalog.", 0, 0);
        goto out;
    }
    S.sn = 0;
    const bool online = net_up();
    int live = 0, kept = 0;
    for (int src = 0; src < SRC_N; src++) {
        char path[48];
        uint32_t len = 0;
        keep_path(src, path, sizeof(path));
        const ls_fetch_req_t q = {.url = CAT_URL[src], .buf = buf, .cap = CAT_CAP, .max_bytes = CAT_CAP - 1,
                                  .timeout_s = 30};
        /* A captive portal answers 200 with a web page, and a cut or broken
           body does not parse: neither is a catalog, and neither may replace
           the kept one. Kept only once it parsed into apps. */
        const int before = S.sn;
        if (online && ls_fetch_run(&q, &len, NULL) == LS_FETCH_OK && buf[strspn(buf, " \r\n\t")] == '{') {
            buf[len] = 0;
            parse(buf, len, src);
            if (S.sn > before) {
                live++;
                mkdir("/sdcard/lakeshark", 0777);
                mkdir(KEEP_DIR, 0777);
                keep_write(path, buf, len);
                continue;
            }
        }
        FILE *f = fopen(path, "rb");
        len = f ? (uint32_t)fread(buf, 1, CAT_CAP - 1, f) : 0;
        if (f) fclose(f);
        kept += len > 0;
        if (len) { buf[len] = 0; parse(buf, len, src); }
    }
    if (live || kept) {
        ls_store_lock();
        memcpy(S.apps, S.stage, sizeof(ls_store_app_t) * (size_t)S.sn);
        S.n = S.sn;
        S.loaded = true;
        S.live = live > 0;
        ls_store_unlock();
    }
    if (live) say(false, "Catalog fresh from terminalbay.com.", 0, 0);
    else if (kept) say(true, online ? "Store unreachable: the list kept on the card." : "Offline: the list kept on the card.", 0, 0);
    else say(true, online ? "Could not reach the store. Try RELOAD." : "No Wi-Fi: connect in Settings, or use the desktop.", 0, 0);
out:
    PS_FREE(buf);
    PS_FREE(S.stage);
    S.stage = NULL;
}

static void progress(uint32_t got, uint32_t total, void *ctx)
{
    (void)ctx;
    S.got = got;
    if (total) S.total = total;
}

static void do_install(const char *id)
{
    char have[16], why[96], url[128], dest[96];
    ls_store_lock();
    const int i = ls_store_find(id);
    ls_store_app_t a;
    ls_store_state_t st = LS_ST_NEW;
    if (i >= 0) { a = S.apps[i]; st = ls_store_state(i, have, sizeof(have)); }
    ls_store_unlock();
    if (i < 0) { say(true, "%s is not in the catalog.", id, 0); return; }
    if (st == LS_ST_BUILTIN) { say(true, "%s is a built-in app's name.", a.name, 0); return; }
    if (st == LS_ST_NEEDS_FW) { say(true, "%s needs firmware %s: update the firmware first.", a.name, a.minfw); return; }
    if (st == LS_ST_INSTALLED) { say(false, "%s %s is up to date.", a.name, a.ver); return; }
#ifdef ESP_PLATFORM
    struct stat sd;
    if (stat("/sdcard", &sd) || (mkdir("/sdcard/apps", 0777) && stat("/sdcard/apps", &sd))) {
        say(true, "No SD card: apps install to the card.", 0, 0);
        return;
    }
#endif
    if (!net_up()) { say(true, "No Wi-Fi: connect in Settings, or install from the desktop.", 0, 0); return; }
#ifdef ESP_PLATFORM
    const ls_lap_info_t *in = ls_lap_by_id(a.id);
    if (in && in->installed && !strncmp(in->path, "/spiffs/", 8)) {   /* the loader refuses it too */
        say(true, "%s comes with the firmware: it updates with the firmware.", a.name, 0);
        return;
    }
    /* An app that is open, or running in the background, cannot be replaced:
       say so before the download, not after it. */
    if (ls_lap_is_open(a.id)) {
        say(true, "%s is running: close it or STOP it in RADIOS, then update.", a.name, 0);
        return;
    }
#endif
    S.phase = LS_SJ_DOWNLOAD;
    S.got = 0;
    S.total = a.size;
    snprintf(url, sizeof(url), "%s%s", BASE[a.src], a.file);
    const ls_fetch_req_t q = {.url = url, .dest = DL_TMP, .bytes = a.size, .max_bytes = LS_LAP_MAX_BYTES,
                              .sha256 = a.sha, .timeout_s = 300, .progress = progress};
    int http = 0;
    const ls_fetch_result_t r = ls_fetch_run(&q, NULL, &http);
    if (r != LS_FETCH_OK) {
        say(true, "Download failed: %s.", ls_fetch_result_text(r), 0);
        return;
    }
    S.phase = LS_SJ_VERIFY;
    snprintf(dest, sizeof(dest), LS_STORE_DIR "/%s.lap", a.id);
#ifdef ESP_PLATFORM
    for (int k = 0; k < 50 && ls_lap_catalog_busy(); k++) vTaskDelay(pdMS_TO_TICKS(100));
    const ls_lap_info_t *l = ls_lap_by_id(a.id);
    if (l && l->installed) snprintf(dest, sizeof(dest), "%s", l->path);   /* an update keeps its file name */
#endif
    if (!ls_lap_install_file(DL_TMP, dest, a.size, a.sha, true, why, sizeof(why))) {
        remove(DL_TMP);
        say(true, "Not installed: %s.", why, 0);
        return;
    }
    say(false, have[0] ? "Updated %s to %s." : "Installed %s %s.", a.name, a.ver);
}

static void do_remove(const char *id)
{
    char why[64];
    S.phase = LS_SJ_REMOVE;
    const int i = ls_store_find(id);
    const char *name = i >= 0 ? S.apps[i].name : id;
    if (ls_lap_remove(id, false, why, sizeof(why))) say(false, why[0] ? "%s %s." : "Removed %s.", name, why);
    else say(true, "%s not removed: %s.", name, why);
}

static bool run_one(void)
{
    job_t j;
    ls_store_lock();
    const bool have = S.qn > 0;
    if (have) {
        j = S.q[S.qh];
        S.qh = (S.qh + 1) % JOBS;
        S.qn--;
        S.busy = true;
        snprintf(S.job_id, sizeof(S.job_id), "%s", j.id);
    }
    ls_store_unlock();
    if (!have) return false;
    if (j.op == J_REFRESH) do_refresh();
    else if (j.op == J_INSTALL) do_install(j.id);
    else if (j.op == J_REMOVE) do_remove(j.id);
    S.phase = LS_SJ_IDLE;
    S.job_id[0] = 0;
    S.busy = false;
    return true;
}

#ifdef ESP_PLATFORM
static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
        while (run_one()) {}
    }
}
#endif

static bool queue(int op, const char *id)
{
    init();
    ls_store_lock();
    bool ok = S.qn < JOBS;
    for (int k = 0; ok && k < S.qn; k++) {   /* the same job twice is one job */
        const job_t *q = &S.q[(S.qh + k) % JOBS];
        if (q->op == op && !strcmp(q->id, id ? id : "")) { ls_store_unlock(); return true; }
    }
    if (ok) {
        job_t *q = &S.q[(S.qh + S.qn) % JOBS];
        q->op = (uint8_t)op;
        snprintf(q->id, sizeof(q->id), "%s", id ? id : "");
        S.qn++;
    }
    ls_store_unlock();
    if (!ok) return false;
#ifdef ESP_PLATFORM
    if (!s_task && xTaskCreatePinnedToCoreWithCaps(worker, "ls_store", 16384, NULL, 2, &s_task, tskNO_AFFINITY,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        say(true, "Could not start the store worker.", 0, 0);
        return false;
    }
    xSemaphoreGive(s_wake);
#else
    while (run_one()) {}
#endif
    return true;
}

bool ls_store_refresh(void) { return queue(J_REFRESH, NULL); }
bool ls_store_install(const char *id) { return ls_store_find(id) >= 0 && queue(J_INSTALL, id); }
bool ls_store_remove(const char *id) { return queue(J_REMOVE, id); }

int ls_store_update_all(void)
{
    int n = 0;
    char have[16];
    for (int i = 0; i < S.n; i++)
        if (ls_store_state(i, have, sizeof(have)) == LS_ST_UPDATE) n += queue(J_INSTALL, S.apps[i].id);
    return n;
}

void ls_store_open(void)
{
    if (!S.opened || (!S.live && !S.busy && ls_fetch_online())) {
        S.opened = true;
        ls_store_refresh();
#ifndef ESP_PLATFORM
        void ls_store_host_opened(void);
        ls_store_host_opened();
#endif
    }
}

#ifndef ESP_PLATFORM
void ls_store_sim_job(const char *id, uint32_t got, uint32_t total)
{
    S.busy = true;
    S.phase = LS_SJ_DOWNLOAD;
    snprintf(S.job_id, sizeof(S.job_id), "%s", id);
    S.got = got;
    S.total = total;
}
#else
/* ------------------------------------------------------------ console -- */

static void wait_jobs(void)
{
    uint32_t last = 0;
    for (int t = 0; t < 3000 && (S.busy || S.qn); t++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (S.phase == LS_SJ_DOWNLOAD && S.total && t % 20 == 0 && S.got != last) {
            last = S.got;
            printf("STORE .. %s %lu/%lu\n", S.job_id, (unsigned long)S.got, (unsigned long)S.total);
        }
    }
    printf("STORE %s %s\n", S.msg_bad ? "ERR" : "OK", S.msg);
}

static int command(int argc, char **argv)
{
    static const char *const ST[] = {"new", "installed", "update", "needs-fw", "builtin"};
    const char *v = argc > 1 ? argv[1] : "list";
    if (!S.loaded && (!strcmp(v, "list") || !strcmp(v, "install") || !strcmp(v, "update"))) {
        ls_store_refresh();
        wait_jobs();
    }
    if (!strcmp(v, "list")) {
        for (int i = 0; i < S.n; i++) {
            char line[200], have[16], caps[24];
            ls_store_lock();   /* a row at a time: the UI keeps drawing while the console prints */
            const ls_store_app_t *a = &S.apps[i];
            const ls_store_state_t st = ls_store_state(i, have, sizeof(have));
            ls_store_caps_text(a->caps, caps, sizeof(caps));
            snprintf(line, sizeof(line), "STORE app id=%s version=%s have=%s state=%s size=%lu min=%s src=%s caps=%s",
                     a->id, a->ver, have[0] ? have : "-", ST[st], (unsigned long)a->size, a->minfw[0] ? a->minfw : "-",
                     a->src ? "community" : "lakeshark", caps);
            ls_store_unlock();
            puts(line);
        }
        printf("STORE OK list %d apps fw=%s %s\n", S.n, ls_store_fw(), S.live ? "live" : "kept");
    } else if (!strcmp(v, "refresh")) {
        ls_store_refresh();
        wait_jobs();
    } else if (!strcmp(v, "update")) {
        const int n = ls_store_update_all();
        if (!n) puts("STORE OK nothing to update");
        else wait_jobs();
    } else if (argc == 3 && (!strcmp(v, "install") || !strcmp(v, "remove"))) {
        if (!(v[0] == 'i' ? ls_store_install(argv[2]) : ls_store_remove(argv[2]))) puts("STORE ERR unknown id or queue full");
        else wait_jobs();
    } else {
        puts("STORE ERR use store list|refresh|install <id>|update|remove <id>");
    }
    return 0;
}

void ls_store_console_register(void)
{
    init();
    const esp_console_cmd_t cmd = {.command = "store", .help = "App store: list|refresh|install <id>|update|remove <id>",
                                   .func = command};
    esp_console_cmd_register(&cmd);
}
#endif
