/* See ls_maptiles.h. The card half is portable; the download task is the
   board's. The HTTP here is a small private fetch: when the shared
   downloader (ls_fetch) lands, fetch_tile() is the one function to swap. */
#include "ls_maptiles.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/idf_additions.h"
#include "ls_sdcard.h"
#include "ls_task_reap.h"
#include <utime.h>
#include "../../../main/ls_wifi.h"
#define CACHE_ROOT "/sdcard/maps/cache"
#define MKDIR(p) mkdir(p, 0777)
static const char *root(void) { return CACHE_ROOT; }
static bool card(void) { return ls_sdcard_mounted(); }
#else
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif
static const char *root(void)
{
    const char *e = getenv("LSSIM_TILE_CACHE");
    return e && *e ? e : "build/lssim_sdcard/maps/cache";
}
static bool card(void) { return true; }
#endif

#define TILE_HOST   "https://tiles.terminalbay.com/v1/"
#define BODY_MAX    (768u * 1024u)
#define WANT_MAX    48
#define RETRY_US    (30 * 1000000LL)
#define CAP_DEFAULT (512ull * 1024ull * 1024ull)
#define CLUSTER_EST 16384u     /* a small file still takes a cluster */
#define AREA_TILE_EST (24u * 1024u)  /* a tile, before the card has been counted */

typedef struct { uint8_t z; uint32_t x, y; } tile_id;

static SemaphoreHandle_t s_lock;
static EXT_RAM_BSS_ATTR tile_id s_want[WANT_MAX];
static int s_nwant;
/* Tiles that just failed, so a frame redrawn 25 times a second does not ask
   for them 25 times a second. */
#define MISS_MAX 32
static EXT_RAM_BSS_ATTR struct { tile_id t; int64_t us; } s_miss[MISS_MAX];
static int s_miss_at;
static volatile uint32_t s_serial;
EXT_RAM_BSS_ATTR static ls_maptiles_stats_t s_st;
static struct { double w, s, e, n; int z0, z1; volatile bool on, cancel; } s_job;
static volatile bool s_do_count, s_do_clear;
/* Tiles written at or after this are the last area download's (run_job
   touches the ones it found already there), and a tidy never cuts them. */
static time_t s_keep_from;
/* s_st.fetched when the card was last tidied: an over-the-limit card is
   tidied again only once more tiles have come in. */
static uint32_t s_tidy_fetched;


static void lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static bool valid(int z, uint32_t x, uint32_t y)
{
    return z >= 0 && z <= LS_MAPTILES_MAXZ && x < (1u << z) && y < (1u << z);
}

static void tile_path(char *out, size_t cap, int z, uint32_t x, uint32_t y, const char *ext)
{
    snprintf(out, cap, "%s/%d/%u/%u.%s", root(), z, (unsigned)x, (unsigned)y, ext);
}

uint32_t ls_maptiles_serial(void) { return s_serial; }

#ifdef ESP_PLATFORM
bool ls_maptiles_online(void) { return ls_wifi_sta_connected(); }
#else
bool ls_maptiles_online(void) { return false; }
#endif

static bool missed(tile_id t, int64_t now)
{
    for (int i = 0; i < MISS_MAX; i++)
        if (s_miss[i].us && s_miss[i].t.z == t.z && s_miss[i].t.x == t.x && s_miss[i].t.y == t.y)
            return now - s_miss[i].us < RETRY_US;
    return false;
}

static void start_task(void);

/* Newest first: the tile asked for last is the one on screen now. */
static bool want(tile_id t)
{
    bool queued = false;
    lock();
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_nwant && !queued; i++)
        if (s_want[i].z == t.z && s_want[i].x == t.x && s_want[i].y == t.y) queued = true;
    if (!queued && !missed(t, now)) {
        if (s_nwant == WANT_MAX) memmove(s_want, s_want + 1, sizeof(s_want[0]) * (size_t)--s_nwant);
        s_want[s_nwant++] = t;
        queued = true;
    }
    s_st.queued = s_nwant;
    unlock();
    if (queued) start_task();
    return queued;
}

void ls_maptiles_forget(void)
{
    lock();
    s_nwant = 0;
    s_st.queued = 0;
    unlock();
}

bool ls_maptiles_has(int z, uint32_t x, uint32_t y)
{
    if (!valid(z, x, y) || !card()) return false;
    char path[96];
    struct stat st;
    tile_path(path, sizeof(path), z, x, y, "mvt");
    return stat(path, &st) == 0;
}

ls_mt_result ls_maptiles_get(int z, uint32_t x, uint32_t y, uint8_t *buf, size_t cap,
                             size_t *len, bool want_it)
{
    if (len) *len = 0;
    if (!valid(z, x, y) || !buf || !len) return LS_MT_NONE;
    if (card()) {
        char path[96];
        tile_path(path, sizeof(path), z, x, y, "mvt");
        FILE *f = fopen(path, "rb");
        if (f) {
            const size_t n = fread(buf, 1, cap, f);
            /* A tile bigger than the buffer is not half a tile: it is none. */
            const bool whole = n < cap || fgetc(f) == EOF;
            fclose(f);
            if (!whole) return LS_MT_NONE;
            *len = n;
            return n ? LS_MT_HAVE : LS_MT_EMPTY;
        }
    }
    if (!want_it || !ls_maptiles_online()) return LS_MT_NONE;
    return want((tile_id){ (uint8_t)z, x, y }) ? LS_MT_WAIT : LS_MT_NONE;
}

void ls_maptiles_stats(ls_maptiles_stats_t *out)
{
    if (!out) return;
    lock();
    *out = s_st;
    out->job = s_job.on;
    if (!out->cap) out->cap = CAP_DEFAULT;
    unlock();
}

void ls_maptiles_set_cap(uint64_t bytes)
{
    lock();
    s_st.cap = bytes < (16ull << 20) ? (16ull << 20) : bytes;
    unlock();
}

static double merc_n(double lat)
{
    if (lat > 85.0511) lat = 85.0511;
    if (lat < -85.0511) lat = -85.0511;
    const double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0;
}

static void area_range(double w, double s, double e, double n, int z,
                       uint32_t *x0, uint32_t *x1, uint32_t *y0, uint32_t *y1)
{
    const double k = ldexp(1.0, z), last = k - 1.0;
    double a = (w + 180.0) / 360.0 * k, b = (e + 180.0) / 360.0 * k;
    double c = merc_n(n) * k, d = merc_n(s) * k;
    if (a < 0) a = 0;
    if (b > last) b = last;
    if (c < 0) c = 0;
    if (d > last) d = last;
    if (b < a) b = a;
    if (d < c) d = c;
    *x0 = (uint32_t)a; *x1 = (uint32_t)b; *y0 = (uint32_t)c; *y1 = (uint32_t)d;
}

static bool area_ok(double w, double s, double e, double n, int z0, int z1)
{
    return isfinite(w) && isfinite(s) && isfinite(e) && isfinite(n) && w < e && s < n &&
           w >= -180 && e <= 180 && z0 >= 0 && z1 >= z0 && z1 <= LS_MAPTILES_MAXZ;
}

uint32_t ls_maptiles_area_tiles(double w, double s, double e, double n, int z0, int z1)
{
    if (!area_ok(w, s, e, n, z0, z1)) return 0;
    uint64_t total = 0;
    for (int z = z0; z <= z1; z++) {
        uint32_t x0, x1, y0, y1;
        area_range(w, s, e, n, z, &x0, &x1, &y0, &y1);
        total += (uint64_t)(x1 - x0 + 1) * (y1 - y0 + 1);
        if (total > 0x7fffffffu) return 0x7fffffffu;
    }
    return (uint32_t)total;
}

static void note(const char *text)
{
    lock();
    snprintf(s_st.note, sizeof(s_st.note), "%s", text);
    unlock();
}

bool ls_maptiles_download_area(double w, double s, double e, double n, int z0, int z1)
{
    if (z1 > LS_MAPTILES_AREA_MAXZ) z1 = LS_MAPTILES_AREA_MAXZ;
    const uint32_t total = ls_maptiles_area_tiles(w, s, e, n, z0, z1);
    if (!total) { note("nothing to download here"); return false; }
    if (total > LS_MAPTILES_AREA_MAX) { note("area too big: zoom in first"); return false; }
    if (!card()) { note("no SD card for the tiles"); return false; }
    if (!ls_maptiles_online()) { note("connect Wi-Fi to download"); return false; }
    if (s_job.on) { note("a download is already running"); return false; }
    lock();
    /* An area must fit the cache with room to spare, or the tidy after it
       has nothing left to cut but the area. Sized by the card's own average
       tile once counted. */
    const uint64_t cap = s_st.cap ? s_st.cap : CAP_DEFAULT;
    const uint64_t per = (s_st.counted && s_st.files ? s_st.bytes / s_st.files : AREA_TILE_EST) + CLUSTER_EST / 2;
    if ((uint64_t)total * per > cap - cap / 4) {
        unlock();
        note("area too big for the tile cache");
        return false;
    }
    /* FAT stamps to two seconds, rounded down: start the kept span early. */
    const time_t now = time(NULL);
    s_keep_from = now > 1600000000 ? now - 2 : 0;
    s_job.w = w; s_job.s = s; s_job.e = e; s_job.n = n; s_job.z0 = z0; s_job.z1 = z1;
    s_job.cancel = false;
    s_st.job_done = s_st.job_failed = 0;
    s_st.job_total = total;
    snprintf(s_st.note, sizeof(s_st.note), "downloading %u tiles", (unsigned)total);
    s_job.on = true;
    unlock();
    start_task();
    return true;
}

void ls_maptiles_cancel(void) { if (s_job.on) s_job.cancel = true; }
void ls_maptiles_count(void) { s_do_count = true; start_task(); }
void ls_maptiles_clear(void) { ls_maptiles_cancel(); s_do_clear = true; start_task(); }

/* ------------------------------------------------------------ the card -- */

static bool all_digits(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) if (*s < '0' || *s > '9') return false;
    return true;
}

/* Walk every tile: count them, and with `cut` delete those last written at
   or before it (0 deletes nothing, -1 deletes all). Returns the oldest and
   newest write times seen through lo/hi. */
static void walk(time_t cut, bool wipe, uint32_t *files, uint64_t *bytes, time_t *lo, time_t *hi)
{
    char zp[64], xp[80], fp[112];
    *files = 0; *bytes = 0;
    if (lo) *lo = 0;
    if (hi) *hi = 0;
    DIR *zd = opendir(root());
    if (!zd) return;
    struct dirent *ze;
    while ((ze = readdir(zd)) != NULL) {
        if (!all_digits(ze->d_name) || strlen(ze->d_name) > 2) continue;
        snprintf(zp, sizeof(zp), "%s/%.2s", root(), ze->d_name);
        DIR *xd = opendir(zp);
        if (!xd) continue;
        struct dirent *xe;
        while ((xe = readdir(xd)) != NULL) {
            if (!all_digits(xe->d_name) || strlen(xe->d_name) > 7) continue;
            snprintf(xp, sizeof(xp), "%s/%.7s", zp, xe->d_name);
            DIR *yd = opendir(xp);
            if (!yd) continue;
            struct dirent *ye;
            uint32_t kept = 0;
            while ((ye = readdir(yd)) != NULL) {
                const size_t n = strlen(ye->d_name);
                if (n < 5 || n > 16 || ye->d_name[0] == '.') continue;
                snprintf(fp, sizeof(fp), "%s/%.16s", xp, ye->d_name);
                struct stat st;
                if (stat(fp, &st) != 0) continue;
                const bool tile = !strcmp(ye->d_name + n - 4, ".mvt");
                if (wipe || !tile || (cut && st.st_mtime <= cut)) { unlink(fp); continue; }
                kept++;
                (*files)++;
                *bytes += (uint64_t)st.st_size;
                if (lo && (!*lo || st.st_mtime < *lo)) *lo = st.st_mtime;
                if (hi && st.st_mtime > *hi) *hi = st.st_mtime;
            }
            closedir(yd);
            if (!kept) rmdir(xp);
#ifdef ESP_PLATFORM
            vTaskDelay(1);
#endif
        }
        closedir(xd);
        if (wipe) rmdir(zp);
    }
    closedir(zd);
}

static uint64_t held(uint32_t files, uint64_t bytes) { return bytes + (uint64_t)files * (CLUSTER_EST / 2); }

/* Count the card, and when it holds more than it may, delete the oldest
   tiles until it is a fifth under. Oldest written: FAT keeps no read time.
   It never deletes everything: tiles that cannot be ordered (a card written
   with no clock, all one age) or that the last area download wrote stay,
   over the limit if need be, rather than taking a saved area with them. */
static void tidy(bool wipe)
{
    uint32_t files = 0; uint64_t bytes = 0; time_t lo = 0, hi = 0;
    walk(0, wipe, &files, &bytes, &lo, &hi);
    lock();
    const uint64_t cap = s_st.cap ? s_st.cap : CAP_DEFAULT;
    const time_t keep = s_keep_from;
    unlock();
    const uint64_t target = cap - cap / 5;
    if (!wipe && held(files, bytes) > cap) {
        /* The fraction to drop, taken off the old end of the age range, a
           pass at a time until under. Every pass deletes at least the oldest
           tiles (cut >= lo), so it ends. */
        for (int pass = 0; pass < 16 && held(files, bytes) > target && hi > lo; pass++) {
            const double over = 1.0 - (double)target / (double)held(files, bytes);
            time_t cut = lo + (time_t)((double)(hi - lo) * (over < 0.1 ? 0.1 : over));
            if (keep && cut >= keep) cut = keep - 1;
            if (cut < lo) break;          /* only the kept area is left */
            walk(cut, false, &files, &bytes, &lo, &hi);
        }
    }
    lock();
    s_st.files = files; s_st.bytes = bytes; s_st.counted = true;
    s_tidy_fetched = s_st.fetched;
    if (wipe) snprintf(s_st.note, sizeof(s_st.note), "tile cache cleared");
    else if (held(files, bytes) > cap) snprintf(s_st.note, sizeof(s_st.note), "tile cache over its limit");
    unlock();
    if (wipe) s_serial++;
}

static bool save_tile(tile_id t, const uint8_t *body, size_t len)
{
    if (!card()) return false;
    char path[96], tmp[96];
    snprintf(path, sizeof(path), "%s", root());
    /* /sdcard/maps may not exist on a new card. */
    char *slash = strrchr(path, '/');
    if (slash) { *slash = 0; MKDIR(path); *slash = '/'; }
    MKDIR(path);
    snprintf(path, sizeof(path), "%s/%d", root(), t.z);
    MKDIR(path);
    snprintf(path, sizeof(path), "%s/%d/%u", root(), t.z, (unsigned)t.x);
    MKDIR(path);
    tile_path(path, sizeof(path), t.z, t.x, t.y, "mvt");
    tile_path(tmp, sizeof(tmp), t.z, t.x, t.y, "tmp");
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    const bool wrote = !len || fwrite(body, 1, len, f) == len;
    if (fclose(f) != 0 || !wrote) { unlink(tmp); return false; }
    unlink(path);
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    lock();
    s_st.files++;
    s_st.bytes += len;
    unlock();
    return true;
}

/* --------------------------------------------------------- the fetching -- */
#ifdef ESP_PLATFORM

static esp_http_client_handle_t s_http;
static uint8_t *s_body;
static size_t s_body_len;
static bool s_body_over;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        if (s_body_len + (size_t)e->data_len > BODY_MAX) s_body_over = true;
        else { memcpy(s_body + s_body_len, e->data, (size_t)e->data_len); s_body_len += (size_t)e->data_len; }
    }
    return ESP_OK;
}

static void http_drop(void)
{
    if (s_http) { esp_http_client_close(s_http); esp_http_client_cleanup(s_http); s_http = NULL; }
}

/* 1 saved, 0 nothing saved this time, -1 the link is down. One connection is
   kept open across tiles: the handshake costs more than a tile does. */
static int fetch_tile(tile_id t)
{
    if (!ls_wifi_sta_connected()) return -1;
    if (!s_body) s_body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_body) return 0;
    char url[96];
    snprintf(url, sizeof(url), TILE_HOST "%d/%u/%u.mvt", t.z, (unsigned)t.x, (unsigned)t.y);
    if (!s_http) {
        const esp_http_client_config_t cfg = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 8000, .buffer_size = 4096, .keep_alive_enable = true,
            .event_handler = on_http, .user_agent = "LakeShark-map/1" };
        s_http = esp_http_client_init(&cfg);
        if (!s_http) return 0;
    } else esp_http_client_set_url(s_http, url);
    s_body_len = 0;
    s_body_over = false;
    const esp_err_t err = esp_http_client_perform(s_http);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(s_http) : 0;
    if (err != ESP_OK) { http_drop(); return ls_wifi_sta_connected() ? 0 : -1; }
    if (s_body_over || (status != 200 && status != 204)) return 0;
    if (status == 204) s_body_len = 0;
    /* a captive portal or an error page answered 200: not a tile, and a saved one is never fetched again */
    if (s_body_len && (s_body[0] == '<' || s_body[0] == '{')) return 0;
    return save_tile(t, s_body, s_body_len) ? 1 : 0;
}

static void miss(tile_id t)
{
    lock();
    s_miss[s_miss_at].t = t;
    s_miss[s_miss_at].us = esp_timer_get_time();
    s_miss_at = (s_miss_at + 1) % MISS_MAX;
    s_st.failed++;
    unlock();
}

static bool take_want(tile_id *t)
{
    lock();
    const bool got = s_nwant > 0;
    if (got) *t = s_want[--s_nwant];
    s_st.queued = s_nwant;
    unlock();
    return got;
}

#ifdef ESP_PLATFORM
extern bool ls_music_live(void);
#endif
/* Card and Wi-Fi work waits while a track plays: it would stall the decoder. */
static void music_wait(void)
{
#ifdef ESP_PLATFORM
    while (ls_music_live() && !s_job.cancel) vTaskDelay(pdMS_TO_TICKS(500));
#endif
}

static void view_wants(void)
{
    tile_id t;
    while (take_want(&t)) {
        music_wait();
        const int r = ls_maptiles_has(t.z, t.x, t.y) ? 1 : fetch_tile(t);
        if (r == 1) { lock(); s_st.fetched++; unlock(); s_serial++; }
        else { miss(t); if (r < 0) { ls_maptiles_forget(); return; } vTaskDelay(pdMS_TO_TICKS(300)); }
    }
}

static void run_job(void)
{
    int fails = 0;
    for (int z = s_job.z0; z <= s_job.z1 && !s_job.cancel; z++) {
        uint32_t x0, x1, y0, y1;
        area_range(s_job.w, s_job.s, s_job.e, s_job.n, z, &x0, &x1, &y0, &y1);
        for (uint32_t x = x0; x <= x1 && !s_job.cancel; x++)
            for (uint32_t y = y0; y <= y1 && !s_job.cancel; y++) {
                view_wants();            /* what is on screen goes first */
                music_wait();
                const tile_id t = { (uint8_t)z, x, y };
                int r = ls_maptiles_has(z, x, y) ? 2 : fetch_tile(t);
                /* A tile the area already had is part of it now: stamp it
                   so the tidy after the job keeps it with the rest. */
                if (r == 2) {
                    char p[112];
                    tile_path(p, sizeof(p), z, x, y, "mvt");
                    (void)utime(p, NULL);
                }
                if (r == 0) { vTaskDelay(pdMS_TO_TICKS(500)); r = fetch_tile(t); }
                lock();
                s_st.job_done++;
                if (r <= 0) s_st.job_failed++;
                if (r == 1) s_st.fetched++;
                unlock();
                fails = r <= 0 ? fails + 1 : 0;
                if (r < 0 || fails >= 8) { s_job.cancel = true; note(r < 0 ? "Wi-Fi lost: download stopped" : "server not answering: stopped"); }
                if ((s_st.job_done & 15) == 0) vTaskDelay(1);
            }
    }
    lock();
    if (!s_job.cancel)
        snprintf(s_st.note, sizeof(s_st.note), "area saved: %u tiles", (unsigned)(s_st.job_done - s_st.job_failed));
    else if (!strstr(s_st.note, "stopped")) snprintf(s_st.note, sizeof(s_st.note), "download stopped");
    s_job.on = false;
    unlock();
    s_serial++;
    s_do_count = true;
}

static volatile bool s_task_on;

static void task(void *arg)
{
    (void)arg;
    int idle = 0;
    while (idle < 100) {                 /* ten quiet seconds and it goes away */
        if (s_do_clear) { s_do_clear = false; s_do_count = false; tidy(true); idle = 0; }
        /* The limit is kept as tiles come in: counted once this boot has
           fetched a few, and tidied whenever the count passes it. */
        if (!s_do_count && !s_job.on &&
            ((!s_st.counted && s_st.fetched >= 64) ||
             (s_st.counted && s_st.fetched - s_tidy_fetched >= 64 &&
              held(s_st.files, s_st.bytes) > (s_st.cap ? s_st.cap : CAP_DEFAULT))))
            s_do_count = true;
        if (s_do_count) { s_do_count = false; tidy(false); idle = 0; }
        if (s_nwant) { view_wants(); idle = 0; }
        if (s_job.on) { run_job(); idle = 0; }
        vTaskDelay(pdMS_TO_TICKS(100));
        idle++;
    }
    http_drop();
    heap_caps_free(s_body);
    s_body = NULL;
    s_task_on = false;
    /* Made with caps: a plain vTaskDelete would leak its TCB and stack. */
    ls_task_retire_self();
}

static void start_task(void)
{
    lock();
    const bool start = !s_task_on;
    if (start) s_task_on = true;
    unlock();
    /* The stack is in PSRAM: this task reads the card and the network and
       never writes flash. */
    if (start && xTaskCreateWithCaps(task, "maptiles", 12288, NULL, 2, NULL,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        s_task_on = false;
}

#else  /* host: the card half only; counting and clearing happen at once */

static void start_task(void)
{
    (void)save_tile;
    if (s_do_clear) { s_do_clear = false; s_do_count = false; tidy(true); }
    if (s_do_count) { s_do_count = false; tidy(false); }
    if (s_job.on) { s_job.on = false; note("no network in the simulator"); }
}

#endif
