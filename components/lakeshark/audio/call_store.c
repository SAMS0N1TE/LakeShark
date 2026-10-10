#include "call_store.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0775)
#endif

/* Lease before fopen, release only after decoder fclose, including queued
 * and paused files. Deletion and lease acquisition share one mutex. */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
static StaticSemaphore_t s_lease_memory;
static SemaphoreHandle_t s_lease_lock;
static unsigned s_lease_once;
EXT_RAM_BSS_ATTR static struct { FILE *file; char path[512]; } s_leases[8];
static void lease_lock(void)
{
    unsigned expected = 0;
    if (__atomic_compare_exchange_n(&s_lease_once, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        s_lease_lock = xSemaphoreCreateMutexStatic(&s_lease_memory);
        __atomic_store_n(&s_lease_once, 2, __ATOMIC_RELEASE);
    } else while (__atomic_load_n(&s_lease_once, __ATOMIC_ACQUIRE) != 2) vTaskDelay(1);
    xSemaphoreTake(s_lease_lock, portMAX_DELAY);
}
FILE *call_store_playback_open(const char *path)
{
    if (!path || strlen(path) >= sizeof(s_leases[0].path)) return NULL;
    lease_lock();
    FILE *file = NULL;
    for (unsigned i = 0; i < 8; ++i) if (!s_leases[i].file) {
        file = fopen(path, "rb");
        if (file) { strcpy(s_leases[i].path, path); s_leases[i].file = file; }
        break;
    }
    xSemaphoreGive(s_lease_lock);
    return file;
}
void call_store_playback_close(FILE *file)
{
    if (!file) return;
    lease_lock();
    fclose(file);
    for (unsigned i = 0; i < 8; ++i) if (s_leases[i].file == file) {
        s_leases[i].file = NULL; s_leases[i].path[0] = 0; break;
    }
    xSemaphoreGive(s_lease_lock);
}
static bool directory(const char *path)
{
    struct stat st;
    return make_dir(path) == 0 || (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}
static bool header(FILE *f, uint32_t rate, uint32_t bytes)
{
    uint8_t h[44]; call_wav_header(h, rate, bytes);
    return fwrite(h, 1, 44, f) == 44;
}
static void finish(call_writer_t *w, bool discard)
{
    if (!w->file) return;
    uint32_t ms = (uint32_t)((uint64_t)w->bytes * 1000 / (w->meta.rate * 2));
    bool ok = !discard && !w->failed && ms >= w->meta.minimum_ms && w->bytes;
    if (fseek(w->file, 0, SEEK_SET) || !header(w->file, w->meta.rate, w->bytes)) ok = false;
    if (fclose(w->file)) ok = false;
    w->file = NULL;
    char side[CALL_PATH_MAX + 8];
    snprintf(side, sizeof(side), "%s.meta", w->path);
    if (ok) {
        FILE *f = fopen(side, "w");
        if (!f) ok = false;
        else {
            int n = fprintf(f, "%lld,%lu,%lu,%lu,%lu,%lu,%u,%.8f,%.8f\n",
                (long long)w->meta.time, (unsigned long)ms,
                (unsigned long)w->meta.hz, (unsigned long)w->meta.talkgroup,
                (unsigned long)w->meta.source, (unsigned long)w->meta.rate,
                w->meta.gps, w->meta.lat, w->meta.lon);
            if (n < 0) ok = false;
            if (fclose(f)) ok = false;
        }
        if (ok && rename(w->temporary, w->path)) ok = false;
    }
    if (!ok) { remove(w->temporary); remove(side); if (!discard) w->errors++; }
}
void call_store_close(call_writer_t *w) { finish(w, false); }
static void begin(call_writer_t *w, const call_meta_t *meta, const char *root)
{
    finish(w, true); w->meta = *meta; w->bytes = 0; w->failed = false;
    if (!meta->rate || !directory(root)) { w->errors++; return; }
    time_t stamp = (time_t)meta->time;
    struct tm tm = {0};
    if (stamp > 0) {
        if (!call_utc_tm(stamp, &tm)) { w->errors++; return; }
    }
    char day[16], clock[24], dir[CALL_PATH_MAX];
    if (stamp > 0) { strftime(day, sizeof(day), "%Y%m%d", &tm); strftime(clock, sizeof(clock), "%H%M%S", &tm); }
    else { strcpy(day, "00000000"); strcpy(clock, "unknown"); }
    snprintf(dir, sizeof(dir), "%s/%s", root, day);
    if (!directory(dir)) { w->errors++; return; }
    for (unsigned collision = 0; collision < 10000; ++collision) {
        char suffix[16] = "";
        if (collision) snprintf(suffix, sizeof(suffix), "_%03u", collision);
        int n = snprintf(w->path, sizeof(w->path), "%s/%s_%lu%s.wav", dir, clock,
            (unsigned long)(meta->talkgroup ? meta->talkgroup : meta->hz), suffix);
        if (n < 0 || (size_t)n + 5 >= sizeof(w->temporary)) break;
        memcpy(w->temporary, w->path, (size_t)n);
        memcpy(w->temporary + n, ".part", 6);
        struct stat st;
        if (!stat(w->path, &st) || !stat(w->temporary, &st)) continue;
        w->file = fopen(w->temporary, "wb");
        if (w->file) {
            /* Coalesce small vocoder blocks into sector-sized writes without
               allocating stdio's default buffer from internal RAM. fclose
               still flushes PCM before the completed call is published. */
            setvbuf(w->file, w->buffer, _IOFBF, sizeof(w->buffer));
            if (!header(w->file, meta->rate, 0)) w->failed = true;
        }
        if (!w->file) w->errors++;
        return;
    }
    w->errors++;
}
bool call_store_step(call_writer_t *w, call_recorder_t *r, const char *root)
{
    const call_block_t *b = call_recorder_peek(r);
    if (!b) return false;
    switch (b->event) {
    case CALL_BEGIN: begin(w, &b->meta, root); break;
    case CALL_PCM:
        if (w->file && !w->failed) {
            if (w->bytes > UINT32_MAX - 36 - b->count * 2) w->failed = true;
            else {
                size_t n = fwrite(b->pcm, 2, b->count, w->file);
                w->bytes += (uint32_t)n * 2;
                if (n != b->count) w->failed = true;
            }
        }
        break;
    case CALL_END: finish(w, false); break;
    case CALL_ABORT: finish(w, true); break;
    }
    call_recorder_consume(r);
    return true;
}
static bool day_name(const char *s)
{
    if (strlen(s) != 8) return false;
    for (int i = 0; i < 8; ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
static bool wav_name(const char *s)
{
    size_t n = strlen(s);
    if (n < 5 || strcmp(s + n - 4, ".wav")) return false;
    for (size_t i = 0; i < n - 4; ++i)
        if (!(s[i] == '_' || (s[i] >= '0' && s[i] <= '9') ||
            (s[i] >= 'a' && s[i] <= 'z'))) return false;
    return true;
}
static bool valid_path(const char *root, const char *path)
{
    size_t n = strlen(root);
    if (strncmp(path, root, n) || path[n] != '/') return false;
    const char *day = path + n + 1, *file = strchr(day, '/');
    if (!file || file - day != 8) return false;
    char name[9]; memcpy(name, day, 8); name[8] = 0;
    if (!day_name(name) || !wav_name(file + 1)) return false;
    return true;
}
bool call_store_protect(const char *root,const char *path,bool protect) {
    if(!valid_path(root,path))return false;
    struct stat st;if(stat(path,&st) || !S_ISREG(st.st_mode))return false;
    char marker[CALL_PATH_MAX+8];snprintf(marker,sizeof(marker),"%s.keep",path);
    if(!protect)return !remove(marker) || errno==ENOENT;
    FILE *f=fopen(marker,"wb");return f && fclose(f)==0;
}
bool call_store_delete(const char *root,const char *path) {
    if(!valid_path(root,path))return false;
    lease_lock();
    for (unsigned i = 0; i < 8; ++i) if (s_leases[i].file && !strcmp(s_leases[i].path, path)) {
        xSemaphoreGive(s_lease_lock); return false;
    }
    if (remove(path)) { xSemaphoreGive(s_lease_lock); return false; }
    char side[CALL_PATH_MAX + 8]; snprintf(side, sizeof(side), "%s.meta", path); remove(side);
    snprintf(side,sizeof(side),"%s.keep",path);remove(side);
    xSemaphoreGive(s_lease_lock);
    return true;
}
int call_store_scan(const char *root, call_entry_t *entries, int capacity)
{
    int count = 0;
    DIR *days = opendir(root); if (!days) return 0;
    struct dirent *day;
    while ((day = readdir(days))) {
        if (!day_name(day->d_name)) continue;
        char dir[CALL_PATH_MAX]; snprintf(dir, sizeof(dir), "%s/%.8s", root, day->d_name);
        DIR *files = opendir(dir); if (!files) continue;
        struct dirent *file;
        while ((file = readdir(files))) {
            if (!wav_name(file->d_name)) continue;
            call_entry_t e = {0};
            int n = snprintf(e.path, sizeof(e.path), "%s/%s", dir, file->d_name);
            if (n < 0 || n >= (int)sizeof(e.path)) continue;
            char side[CALL_PATH_MAX + 8]; snprintf(side, sizeof(side), "%s.meta", e.path);
            FILE *f = fopen(side, "r"); if (!f) continue;
            long long stamp; unsigned ms, hz, tg, src, rate, gps;
            int got = fscanf(f, "%lld,%u,%u,%u,%u,%u,%u,%lf,%lf", &stamp, &ms,
                &hz, &tg, &src, &rate, &gps, &e.meta.lat, &e.meta.lon);
            fclose(f); if (got != 9) continue;
            e.meta.time = stamp; e.meta.hz = hz; e.meta.talkgroup = tg;
            e.meta.source = src; e.meta.rate = rate; e.meta.gps = gps != 0;
            e.duration_ms = ms;
            snprintf(side,sizeof(side),"%s.keep",e.path);e.kept=access(side,F_OK)==0;
            struct stat st; if (!stat(e.path,&st) && S_ISREG(st.st_mode)) e.bytes=(uint32_t)st.st_size;
            int at = 0;
            while (at < count && (entries[at].meta.time > stamp ||
                (entries[at].meta.time == stamp && strcmp(entries[at].path, e.path) > 0))) ++at;
            if (at >= capacity) continue;
            if (count < capacity) ++count;
            memmove(entries + at + 1, entries + at, (count - at - 1) * sizeof(e));
            entries[at] = e;
        }
        closedir(files);
    }
    closedir(days); return count;
}
/* Civil date to epoch day, rejecting malformed archive directory names. */
static int64_t epoch_day(const char *name)
{
    if (!day_name(name)) return -1;
    unsigned y, m, d;
    if (sscanf(name, "%4u%2u%2u", &y, &m, &d) != 3 || y < 2024 || y > 2099 || m < 1 || m > 12) return -1;
    static const unsigned md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    if (!d || d > md[m - 1] + (m == 2 && leap)) return -1;
    int64_t days = 0;
    for (unsigned year = 1970; year < y; ++year) days += 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    for (unsigned month = 1; month < m; ++month) days += md[month - 1] + (month == 2 && leap);
    return days + d - 1;
}
void call_store_retain(const char *root, int64_t today, unsigned keep)
{
    if (!keep) return;
    DIR *days = opendir(root); if (!days) return;
    struct dirent *day;
    while ((day = readdir(days))) {
        if (!call_day_expired(epoch_day(day->d_name), today, keep)) continue;
        char dir[CALL_PATH_MAX]; snprintf(dir, sizeof(dir), "%s/%.8s", root, day->d_name);
        DIR *files = opendir(dir); if (!files) continue;
        struct dirent *file;
        while ((file = readdir(files))) {
            if (!wav_name(file->d_name)) continue;
            char path[CALL_PATH_MAX];
            int n = snprintf(path, sizeof(path), "%s/%s", dir, file->d_name);
            if (n > 0 && n < (int)sizeof(path)) {
                char marker[CALL_PATH_MAX+8];snprintf(marker,sizeof(marker),"%s.keep",path);
                if(access(marker,F_OK)!=0)call_store_delete(root,path);
            }
        }
        closedir(files);
        /* Unrelated files and unfinished recordings keep their directory. */
        rmdir(dir);
    }
    closedir(days);
}

uint64_t call_store_size(const char *root)
{
    uint64_t bytes = 0;
    DIR *days = opendir(root); if (!days) return 0;
    struct dirent *day;
    while ((day = readdir(days))) {
        if (!day_name(day->d_name)) continue;
        char dir[CALL_PATH_MAX]; snprintf(dir, sizeof(dir), "%s/%.8s", root, day->d_name);
        DIR *files = opendir(dir); if (!files) continue;
        struct dirent *file;
        while ((file = readdir(files))) {
            if (file->d_name[0] == '.') continue;
            char path[CALL_PATH_MAX];
            int n = snprintf(path, sizeof(path), "%s/%s", dir, file->d_name);
            struct stat st;
            if (n > 0 && n < (int)sizeof(path) && !stat(path, &st) && S_ISREG(st.st_mode))
                bytes += (uint64_t)st.st_size;
        }
        closedir(files);
    }
    closedir(days); return bytes;
}
