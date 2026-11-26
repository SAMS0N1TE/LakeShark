/* Bluetooth names for the firmware's screens: see ls_ble_names.h. The file
   format, UUIDs and line rules are app-sdk/lib/lap_ids_fmt.c, the same code
   every store app runs; this file is the firmware's engine around it: plain
   stdio on a worker task instead of the app engine's lap_sd_* jobs. */
#include "ls_ble_names.h"
#include "../../../app-sdk/lib/lap_ids.h"
#include "../../../app-sdk/lib/lap_ids_fmt.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CACHE        256                /* names looked up, kept */
#define BATCH        16                 /* lookups a worker pass */
#define WQ           16                 /* device lines waiting to be appended */
#define WLINE        64
#define IO           LAP_IDS_USER_BYTES /* a text file's tail, read at most */
#define IDX_TOTAL    (64 * 1024)        /* every index together: real ones are a few KB */
#define CHECK_MS     3000               /* the files' size and time, looked at while in use */
#define IDLE_MS      20000              /* nobody asked for this long: the worker sleeps */
#define WRITE_GAP_MS 1000
#define RELEARN_MS   60000              /* a device's name changes at most this often */
#define TOKENS       8                  /* appends in a burst ... */
#define TOKEN_MS     3000               /* ... and one more this often */

enum { C_EMPTY, C_WAIT, C_READING, C_FOUND, C_ABSENT };
typedef struct {
    uint8_t kind, st;
    uint32_t key;
    char name[LAP_IDS_NAME];
} cent_t;

typedef struct {
    uint8_t mac[6];                     /* shown order */
    bool owner;                         /* from ble-names.txt: wins over heard names */
    bool session;                       /* learned since boot */
    uint32_t age, learned_ms;
    char name[LAP_IDS_NAME];
} bdev_t;

typedef struct { long size; long long mtime; bool there; } fstamp_t;

typedef struct {
    /* shared with every caller, under s_lock */
    cent_t cache[CACHE];
    int cache_next;
    lap_ids_user_t *users, *users_alt;
    int nusers;
    bdev_t *devs, *devs_alt;
    int ndevs;
    uint32_t dev_age;
    char wq[WQ][WLINE];
    int wq_head, wq_n;
    int tokens;
    uint32_t tokens_ms;
    bool loaded;
    uint32_t used_ms;
    int file_state;                     /* 0 none, 1 loading, 2 ready */
    uint32_t counts[LAP_IDS_KINDS];
    unsigned learned, held, written, write_fail;
    /* the worker's own */
    fstamp_t fu, fd, fb;
    int path_i, bin_first;              /* the name file read; the first one there when looked */
    uint32_t check_ms, write_ms;
    lap_ids_head_t hd;
    uint8_t *idx;
    char io[IO];
    char wbuf[WQ * WLINE];
    lap_ids_user_t users_a[LAP_IDS_USER_MAX], users_b[LAP_IDS_USER_MAX];
    bdev_t devs_a[LAP_IDS_DEVICES], devs_b[LAP_IDS_DEVICES];
} st_t;

static st_t *S;
static StaticSemaphore_t s_lock_mem;
static SemaphoreHandle_t s_lock;
static unsigned s_once;                 /* 0 not yet, 1 starting, 2 running, 3 no memory */
static TaskHandle_t s_task;
static volatile bool s_parked;

static const char *const DEF_IDS[] = { LAP_IDS_FILE1, LAP_IDS_FILE2, LAP_IDS_FILE3 };
static const char *s_ids_path, *s_user_path, *s_dev_path;
static const char *user_path(void) { return s_user_path ? s_user_path : LAP_IDS_USER_FILE; }
static const char *dev_path(void) { return s_dev_path ? s_dev_path : LAP_IDS_DEVICE_FILE; }
static int ids_paths(void) { return s_ids_path ? 1 : (int)(sizeof(DEF_IDS) / sizeof(DEF_IDS[0])); }
static const char *ids_path(int i) { return s_ids_path ? s_ids_path : DEF_IDS[i]; }

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

/* ------------------------------------------------------------ the task */

#ifdef ESP_PLATFORM
static void task(void *arg)
{
    (void)arg;
    for (;;) {
        ls_ble_names_work();
        lock();
        const bool idle = now_ms() - S->used_ms > IDLE_MS && !S->wq_n;
        unlock();
        s_parked = idle;
        ulTaskNotifyTake(pdTRUE, idle ? portMAX_DELAY : pdMS_TO_TICKS(400));
        s_parked = false;
    }
}

static void spawn(void)
{
    /* Card reads and appends only: no NVS or internal flash, so the stack
       may be PSRAM (as WARDRIVE's worker's is). */
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    if (xTaskCreateWithCaps(task, "ble-names", 5120, NULL, 2, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        s_task = NULL;
#else
    if (xTaskCreate(task, "ble-names", 5120, NULL, 2, &s_task) != pdPASS) s_task = NULL;
#endif
}
static void wake(void) { if (s_task) xTaskNotifyGive(s_task); }
#else
static void spawn(void) {}
static void wake(void) {}
#endif

static bool ensure(void)
{
    unsigned want = 0;
    if (__atomic_compare_exchange_n(&s_once, &want, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
        st_t *n = heap_caps_calloc(1, sizeof(st_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (n && s_lock) {
            n->users = n->users_a; n->users_alt = n->users_b;
            n->devs = n->devs_a; n->devs_alt = n->devs_b;
            n->tokens = TOKENS;
            n->tokens_ms = now_ms();
            n->used_ms = n->tokens_ms;
            n->file_state = 1;
            n->path_i = -1;
            n->bin_first = -2;          /* the name file not looked for yet */
        } else if (n) { heap_caps_free(n); n = NULL; }
        S = n;
        __atomic_store_n(&s_once, n ? 2u : 3u, __ATOMIC_RELEASE);
        if (n) spawn();
    } else {
        while (__atomic_load_n(&s_once, __ATOMIC_ACQUIRE) == 1) vTaskDelay(1);
    }
    return __atomic_load_n(&s_once, __ATOMIC_ACQUIRE) == 2;
}

/* ------------------------------------------------------------ tables */

static bdev_t *dev_find(bdev_t *d, int n, const uint8_t mac[6])
{
    for (int i = 0; i < n; i++) if (!memcmp(d[i].mac, mac, 6)) return &d[i];
    return NULL;
}

/* lap_ids' rule: an owner name is never replaced by a heard one; a full
   table gives up its oldest heard name. */
static bdev_t *dev_put(bdev_t *d, int *n, uint32_t *age, const uint8_t mac[6], const char *name, bool owner)
{
    bdev_t *e = dev_find(d, *n, mac);
    if (e && e->owner && !owner) return NULL;
    if (!e) {
        if (*n < LAP_IDS_DEVICES) e = &d[(*n)++];
        else for (int i = 0; i < *n; i++) if (!d[i].owner && (!e || d[i].age < e->age)) e = &d[i];
        if (!e) return NULL;
        memset(e, 0, sizeof(*e));
        memcpy(e->mac, mac, 6);
    }
    e->owner = owner;
    e->age = ++*age;
    snprintf(e->name, sizeof(e->name), "%s", name);
    return e;
}

static void user_put(lap_ids_user_t *u, int *n, const lap_ids_user_t *x)
{
    for (int i = 0; i < *n; i++) if (u[i].kind == x->kind && u[i].key == x->key) { u[i] = *x; return; }
    if (*n < LAP_IDS_USER_MAX) u[(*n)++] = *x;
}

/* ------------------------------------------------------------ the card (worker only) */

static fstamp_t stamp(const char *path)
{
    fstamp_t s = {0};
    struct stat st;
    if (!stat(path, &st)) { s.size = (long)st.st_size; s.mtime = (long long)st.st_mtime; s.there = true; }
    return s;
}
static bool same(fstamp_t a, fstamp_t b) { return a.there == b.there && a.size == b.size && a.mtime == b.mtime; }

static long read_at(const char *path, long off, void *buf, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = -1;
    if (!fseek(f, off, SEEK_SET)) n = (long)fread(buf, 1, len, f);
    fclose(f);
    return n;
}

/* The device file rewritten from what is held, owner names left out (they
   live in the user file): lap_ids does the same when the file grows. */
static void compact(void)
{
    size_t n = 0;
    lock();
    for (int i = 0; i < S->ndevs && n + 64 < sizeof(S->io); i++) {
        if (S->devs[i].owner) continue;
        char m[18];
        lap_ids_mac_text(S->devs[i].mac, m, sizeof(m));
        n += (size_t)snprintf(S->io + n, sizeof(S->io) - n, "%s %s\n", m, S->devs[i].name);
    }
    unlock();
    FILE *f = fopen(dev_path(), "wb");
    if (!f) return;
    fwrite(S->io, 1, n, f);
    fclose(f);
    S->fd = stamp(dev_path());
}

/* Both text files into the spare tables, then swapped in at once: a screen
   never sees a half-read table. */
static void load_text(fstamp_t fu, fstamp_t fd)
{
    lap_ids_user_t *nu = S->users_alt;
    bdev_t *nd = S->devs_alt;
    int nnu = 0, nnd = 0;
    uint32_t age = 0;
    for (int pass = 0; pass < 2; pass++) {
        const bool devs = pass == 0;    /* heard names first, the owner's over them */
        const fstamp_t s = devs ? fd : fu;
        if (!s.there || s.size <= 0) continue;
        const long off = devs && s.size > (long)IO ? s.size - (long)IO : 0;   /* later lines win: the tail */
        const long n = read_at(devs ? dev_path() : user_path(), off, S->io, IO);
        if (n <= 0) continue;
        long i = 0;
        if (off > 0) { while (i < n && S->io[i] != '\n') i++; i++; }
        while (i < n) {
            long e = i;
            while (e < n && S->io[e] != '\n') e++;
            if (e == n && n == (long)IO) break;                 /* cut by the buffer */
            const char *l = S->io + i;
            const size_t len = (size_t)(e - i);
            if (devs) {
                uint8_t m[6];
                char nm[LAP_IDS_NAME];
                if (lap_ids_device_line(l, len, m, nm, sizeof(nm))) dev_put(nd, &nnd, &age, m, nm, false);
            } else {
                lap_ids_user_t u;
                if (lap_ids_parse_line(l, len, &u) == 1) {
                    if (u.kind == LAP_IDS_DEVICE) dev_put(nd, &nnd, &age, u.mac, u.name, true);
                    else user_put(nu, &nnu, &u);
                }
            }
            i = e + 1;
        }
    }
    lock();
    /* names learned this session the file does not hold yet stay */
    for (int i = 0; i < S->ndevs; i++) {
        const bdev_t *o = &S->devs[i];
        if (!o->session) continue;
        bdev_t *e = dev_put(nd, &nnd, &age, o->mac, o->name, false);
        if (e) { e->session = true; e->learned_ms = o->learned_ms; }
    }
    S->users_alt = S->users; S->users = nu; S->nusers = nnu;
    S->devs_alt = S->devs; S->devs = nd; S->ndevs = nnd; S->dev_age = age;
    S->loaded = true;
    unlock();
    S->fu = fu;
    S->fd = fd;
    if (fd.there && fd.size > 3L * (long)IO) compact();
}

/* The name file: header and indexes into PSRAM; lookups read a block and a
   name each. A new or changed file empties the cache. */
static void load_bin(int from)
{
    heap_caps_free(S->idx);
    S->idx = NULL;
    memset(&S->hd, 0, sizeof(S->hd));
    int fs = 0;
    uint32_t counts[LAP_IDS_KINDS] = {0};
    for (int p = from; p >= 0 && p < ids_paths() && !fs; p++) {
        uint8_t head[LAP_IDS_HEAD_MAX];
        FILE *f = fopen(ids_path(p), "rb");
        if (!f) continue;
        const size_t hn = fread(head, 1, sizeof(head), f);
        lap_ids_head_t h;
        if (!lap_ids_head(&h, head, (uint32_t)hn)) { fclose(f); continue; }
        uint32_t total = 0;
        for (int i = 0; i < h.nt; i++) {
            const uint32_t len = h.t[i].nidx * h.t[i].keyb;
            if (total + len > IDX_TOTAL) h.t[i].nidx = 0;       /* too big: that table is left out */
            else total += len;
        }
        uint8_t *idx = total ? heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        uint32_t at = 0;
        for (int i = 0; i < h.nt && idx; i++) {
            lap_ids_tab_t *t = &h.t[i];
            const uint32_t len = t->nidx * t->keyb;
            if (!len) continue;
            if (fseek(f, (long)t->idx_off, SEEK_SET) || fread(idx + at, 1, len, f) != len) { t->nidx = 0; continue; }
            t->idx = idx + at;
            at += len;
            if (t->kind < LAP_IDS_KINDS) counts[t->kind] = t->count;
        }
        fclose(f);
        if (!idx) continue;
        S->hd = h;
        S->idx = idx;
        S->path_i = p;
        fs = 2;
    }
    lock();
    S->file_state = fs;
    memcpy(S->counts, counts, sizeof(counts));
    /* names from the old file are asked for again; ones waiting stay waiting */
    for (int i = 0; i < CACHE; i++) if (S->cache[i].st != C_WAIT) S->cache[i].st = C_EMPTY;
    unlock();
}

static void check_files(void)
{
    const fstamp_t fu = stamp(user_path()), fd = stamp(dev_path());
    if (!S->loaded || !same(fu, S->fu) || !same(fd, S->fd)) load_text(fu, fd);
    int first = -1;
    fstamp_t fb = {0};
    for (int p = 0; p < ids_paths(); p++) { fb = stamp(ids_path(p)); if (fb.there) { first = p; break; } }
    if (S->bin_first != -2 && first == S->bin_first && same(fb, S->fb)) return;
    S->fb = fb;
    S->bin_first = first;
    S->path_i = -1;
    load_bin(first);
}

static bool lookup(FILE *f, int kind, uint32_t key, char *name)
{
    static const size_t BLK = 64 * 7;   /* a block: every <= 64, key <= 4 */
    uint8_t blk[64 * 7];
    const lap_ids_tab_t *t = lap_ids_table(&S->hd, kind);
    uint32_t first, n;
    if (!t || !t->idx || !lap_ids_block(t, key, &first, &n) || n * t->recb > BLK) return false;
    if (fseek(f, (long)(t->rec_off + first * t->recb), SEEK_SET) || fread(blk, 1, n * t->recb, f) != n * t->recb)
        return false;
    const long off = lap_ids_in_block(t, blk, n, key);
    if (off < 0 || (uint32_t)off >= S->hd.pool_len) return false;
    uint32_t len = S->hd.pool_len - (uint32_t)off;
    if (len > LAP_IDS_NAME) len = LAP_IDS_NAME;
    memset(name, 0, LAP_IDS_NAME);
    if (fseek(f, (long)(S->hd.pool_off + (uint32_t)off), SEEK_SET) || !fread(name, 1, len, f)) return false;
    lap_ids_name_fix(name, LAP_IDS_NAME);
    return name[0] != 0;
}

static void resolve(void)
{
    struct { uint8_t kind; uint32_t key; int slot; } job[BATCH];
    int nj = 0;
    lock();
    for (int i = 0; i < CACHE && nj < BATCH; i++)
        if (S->cache[i].st == C_WAIT) {
            S->cache[i].st = C_READING;
            job[nj].kind = S->cache[i].kind; job[nj].key = S->cache[i].key; job[nj].slot = i;
            nj++;
        }
    const bool ready = S->file_state == 2;
    unlock();
    if (!nj) return;
    FILE *f = ready && S->path_i >= 0 ? fopen(ids_path(S->path_i), "rb") : NULL;
    for (int j = 0; j < nj; j++) {
        char nm[LAP_IDS_NAME];
        const bool ok = f && lookup(f, job[j].kind, job[j].key, nm);
        lock();
        cent_t *c = &S->cache[job[j].slot];
        if (c->st == C_READING && c->kind == job[j].kind && c->key == job[j].key) {
            if (ok) memcpy(c->name, nm, sizeof(c->name));
            c->st = ok ? C_FOUND : C_ABSENT;
        }
        unlock();
    }
    if (f) fclose(f);
}

static void flush(uint32_t now)
{
    if (now - S->write_ms < WRITE_GAP_MS) return;
    size_t n = 0;
    int lines = 0;
    lock();
    while (S->wq_n) {
        n += (size_t)snprintf(S->wbuf + n, sizeof(S->wbuf) - n, "%s\n", S->wq[S->wq_head]);
        S->wq_head = (S->wq_head + 1) % WQ;
        S->wq_n--;
        lines++;
    }
    unlock();
    if (!n) return;
    S->write_ms = now;
    /* an app's append since the last look is still read; ours is not */
    const bool ours = same(stamp(dev_path()), S->fd);
    FILE *f = fopen(dev_path(), "ab");
    const bool ok = f && fwrite(S->wbuf, 1, n, f) == n;
    if (f) fclose(f);
    lock();
    if (ok) S->written += (unsigned)lines; else S->write_fail += (unsigned)lines;
    unlock();
    if (ours) S->fd = stamp(dev_path());
}

void ls_ble_names_work(void)
{
    if (__atomic_load_n(&s_once, __ATOMIC_ACQUIRE) != 2) return;   /* nothing asked yet */
    const uint32_t now = now_ms();
    lock();
    const uint32_t used = S->used_ms;
    const bool loaded = S->loaded;
    unlock();
    if (!loaded || (now - S->check_ms >= CHECK_MS && now - used < IDLE_MS)) {
        S->check_ms = now;
        check_files();
    }
    resolve();
    flush(now);
}

/* ------------------------------------------------------------ callers */

static bool get_name(int kind, uint32_t key, char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = 0;
    if (!ensure()) return false;
    bool ok = false, ask = false;
    lock();
    S->used_ms = now_ms();
    for (int i = 0; i < S->nusers && !ok; i++)
        if (S->users[i].kind == kind && S->users[i].key == key) { snprintf(out, cap, "%s", S->users[i].name); ok = true; }
    int hit = -1;
    for (int i = 0; i < CACHE && !ok && hit < 0; i++)
        if (S->cache[i].st != C_EMPTY && S->cache[i].kind == kind && S->cache[i].key == key) hit = i;
    if (hit >= 0 && S->cache[hit].st == C_FOUND) { snprintf(out, cap, "%s", S->cache[hit].name); ok = true; }
    if (!ok && hit < 0) {
        int i = S->cache_next;
        for (int k = 0; k < CACHE && S->cache[i].st == C_READING; k++) i = (i + 1) % CACHE;
        if (S->cache[i].st != C_READING) {
            S->cache[i].kind = (uint8_t)kind;
            S->cache[i].key = key;
            S->cache[i].st = C_WAIT;
            S->cache[i].name[0] = 0;
            S->cache_next = (i + 1) % CACHE;
            ask = true;
        }
    }
    unlock();
    if (ask || s_parked) wake();
    return ok;
}

bool ls_ble_names_company(uint16_t company, char *out, size_t cap) { return get_name(LAP_IDS_COMPANY, company, out, cap); }
bool ls_ble_names_service(uint16_t uuid16, char *out, size_t cap) { return get_name(LAP_IDS_SERVICE, uuid16, out, cap); }
bool ls_ble_names_appearance(uint16_t a, char *out, size_t cap) { return get_name(LAP_IDS_APPEARANCE, a, out, cap); }

bool ls_ble_names_maker(const uint8_t mac[6], char *out, size_t cap)
{
    if (out && cap) out[0] = 0;
    if (!mac || (mac[0] & 0x02)) return false;      /* locally administered: random, no maker */
    return get_name(LAP_IDS_OUI, (uint32_t)mac[0] << 16 | (uint32_t)mac[1] << 8 | mac[2], out, cap);
}

bool ls_ble_names_device(const uint8_t mac[6], uint8_t type, char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = 0;
    if (!mac || (type != LS_BLE_NAMES_TYPE_UNKNOWN && !lap_ids_keepable(mac, type)) || !ensure()) return false;
    lock();
    S->used_ms = now_ms();
    const bdev_t *d = dev_find(S->devs, S->ndevs, mac);
    if (d) snprintf(out, cap, "%s", d->name);
    unlock();
    if (s_parked) wake();
    return d != NULL;
}

int ls_ble_names_label(const uint8_t mac[6], uint8_t type, int company, int uuid16, char *out, size_t cap)
{
    if (!out || !cap) return LS_BLE_LABEL_NONE;
    if (ls_ble_names_device(mac, type, out, cap)) return LS_BLE_LABEL_NAME;
    if (company >= 0 && company <= 0xFFFF && ls_ble_names_company((uint16_t)company, out, cap)) return LS_BLE_LABEL_GUESS;
    if (uuid16 >= 0 && uuid16 <= 0xFFFF && ls_ble_names_service((uint16_t)uuid16, out, cap)) return LS_BLE_LABEL_GUESS;
    /* a maker only for an address known to be public: a random one's first
       bytes are not a prefix anyone owns */
    const bool pub = type != LS_BLE_NAMES_TYPE_UNKNOWN && !(type & 1);
    if (mac && pub && ls_ble_names_maker(mac, out, cap)) return LS_BLE_LABEL_GUESS;
    out[0] = 0;
    return LS_BLE_LABEL_NONE;
}

int ls_ble_names_learn(const uint8_t mac[6], uint8_t type, const char *name)
{
    char nm[LAP_IDS_NAME];
    if (!mac || !lap_ids_keepable(mac, type) || !lap_ids_heard_name(name, nm, sizeof(nm)) || !ensure()) return 0;
    int kept = 0;
    bool queued = false;
    lock();
    const uint32_t now = now_ms();
    S->used_ms = now;
    /* before the files are read a name could look new that is not: the
       device is heard again in a moment */
    bdev_t *d = S->loaded ? dev_find(S->devs, S->ndevs, mac) : NULL;
    const bool refuse = !S->loaded || (d && (d->owner || !strcmp(d->name, nm))) ||
                        (d && d->session && now - d->learned_ms < RELEARN_MS);
    if (!refuse && (d = dev_put(S->devs, &S->ndevs, &S->dev_age, mac, nm, false)) != NULL) {
        d->session = true;
        d->learned_ms = now;
        kept = 1;
        S->learned++;
        const uint32_t add = (now - S->tokens_ms) / TOKEN_MS;
        if (add) {
            S->tokens = S->tokens + (int)add > TOKENS ? TOKENS : S->tokens + (int)add;
            S->tokens_ms += add * TOKEN_MS;
        }
        if (S->tokens >= TOKENS) S->tokens_ms = now;
        if (S->tokens > 0 && S->wq_n < WQ) {
            char m[18];
            lap_ids_mac_text(mac, m, sizeof(m));
            snprintf(S->wq[(S->wq_head + S->wq_n) % WQ], WLINE, "%s %s", m, nm);
            S->wq_n++;
            S->tokens--;
            queued = true;
        } else S->held++;
    }
    unlock();
    if (queued || s_parked) wake();
    return kept;
}

int ls_ble_names_state(char *out, size_t cap)
{
    if (out && cap) out[0] = 0;
    if (!ensure()) { if (out && cap) snprintf(out, cap, "no memory for names"); return 0; }
    lock();
    S->used_ms = now_ms();
    const int st = S->loaded ? S->file_state : 1;
    if (out && cap) {
        if (st == 2)
            snprintf(out, cap, "%u co %u svc %u makers, %d yours, %d devices",
                     (unsigned)S->counts[LAP_IDS_COMPANY], (unsigned)S->counts[LAP_IDS_SERVICE],
                     (unsigned)S->counts[LAP_IDS_OUI], S->nusers, S->ndevs);
        else if (st == 1) snprintf(out, cap, "loading names");
        else snprintf(out, cap, "no name file: ble-ids from PACKS; %d yours, %d devices", S->nusers, S->ndevs);
    }
    unlock();
    if (s_parked) wake();
    return st;
}

/* ------------------------------------------------------------ bench */

void ls_ble_names_test_paths(const char *ids, const char *user, const char *devices)
{
    if (ids) s_ids_path = ids;
    if (user) s_user_path = user;
    if (devices) s_dev_path = devices;
}

void ls_ble_names_reset(void)
{
    if (S) { heap_caps_free(S->idx); heap_caps_free(S); }
    S = NULL;
    s_task = NULL;
    s_parked = false;
    __atomic_store_n(&s_once, 0u, __ATOMIC_RELEASE);
}
