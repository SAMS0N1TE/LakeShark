/* Firmware modules: see ls_mod.h. */
#include "ls_mod.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(ESP_PLATFORM) && defined(CONFIG_LS_APP_MODULES) && CONFIG_LS_APP_MODULES && __has_include("ls_mod_table.inc")

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core/ls_fetch.h"
#include "core/ls_nvs_safe.h"
#include "esp_attr.h"
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ls_lap.h"
#include "ls_tui_screen.h"
#include "mbedtls/sha256.h"

#define MOD_DIR_INTERNAL "/spiffs/apps"
#define MOD_DIR_CARD "/sdcard/apps"
#define MOD_URL "https://data.terminalbay.com/mod/"
#define MOD_MAX_BYTES (1024u * 1024u)
#define MOD_RETRY_MS 60000u

/* Critical sections of module code: a module's portMUX_TYPE statics would sit in
   PSRAM, so the prelude maps portENTER/EXIT_CRITICAL to these (one internal-RAM
   spinlock; sections are a few instructions, nesting is allowed). */
static portMUX_TYPE s_mod_crit = portMUX_INITIALIZER_UNLOCKED;
void ls_mod_crit_enter(void) { portENTER_CRITICAL(&s_mod_crit); }
void ls_mod_crit_exit(void) { portEXIT_CRITICAL(&s_mod_crit); }

typedef struct {
    const char *id;
    uint32_t size;
    uint8_t sha[32];
} ls_mod_info_t;
typedef struct {
    uint8_t mod, ordinal;
    const ls_tui_screen_t *proxy;
    char *hint;     /* a screen that rewrites its hint: the proxy's copy */
} ls_mod_screen_info_t;
#define LS_MOD_HINT_MAX 128

static void s_enter(int i);
static void s_leave(int i);
static void s_draw(int i, tui_surface *sf, tui_rect area);
static bool s_key(int i, ls_tk_t k, char ch);
static bool s_touch(int i, int col, int row);
#define LS_MOD_SCREEN(n)                                                                                \
    static void ls_mod_s##n##_enter(void) { s_enter(n); }                                               \
    static void ls_mod_s##n##_leave(void) { s_leave(n); }                                               \
    static void ls_mod_s##n##_draw(tui_surface *sf, tui_rect a) { s_draw(n, sf, a); }                   \
    static bool __attribute__((unused)) ls_mod_s##n##_key(ls_tk_t k, char c) { return s_key(n, k, c); } \
    static bool __attribute__((unused)) ls_mod_s##n##_touch(int c, int r) { return s_touch(n, c, r); }
#include "ls_mod_table.inc"

typedef struct {
    const char *name;
    uintptr_t addr;
} export_t;
extern const export_t ls_mod_exports[];
extern const uint32_t ls_mod_exports_count;

enum { M_UNKNOWN, M_STORED, M_ABSENT, M_LOADED, M_FAILED };
static struct {
    uint8_t state;
    bool fetching, fetched_bad, bg;     /* bg: the fetch waits for a quiet moment (nobody asked) */
    uint8_t fails;
    int fetch_id;
    uint32_t next_try_ms;
    const ls_mod_desc_t *desc;
    esp_elf_t elf;
    char *fetch_buf;
    char why[48];
} mods[LS_MOD_COUNT ? LS_MOD_COUNT : 1];
static struct {
    bool shown, entered, asked;
} screens[LS_MOD_SCREEN_COUNT ? LS_MOD_SCREEN_COUNT : 1];
static char s_missing[48];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uintptr_t resolve(const char *name) {
    int lo = 0, hi = (int)ls_mod_exports_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2, c = strcmp(name, ls_mod_exports[mid].name);
        if (!c)
            return ls_mod_exports[mid].addr;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    /* No snprintf here: this runs on the small relocation stack (COMPASS ran it dry). */
    strncpy(s_missing, name, sizeof(s_missing) - 1);
    s_missing[sizeof(s_missing) - 1] = 0;
    return 0;
}

/* ---- file work, on a stack the flash driver accepts (SPIFFS turns the cache off) */
typedef struct {
    int mod;
    uint8_t *data;      /* read: out, PSRAM; write: in */
    uint32_t size;
    uint32_t off;       /* write: where the next step starts */
    bool ok;
} file_job;
/* A write is done in steps of this much, one ls_nvs_run each, so the one
   flash worker is free between them for everybody else. */
#define MOD_WRITE_STEP 8192u
static void path_of(char *out, size_t cap, const char *dir, int m, const char *suffix) {
    snprintf(out, cap, "%s/%s.lsm%s", dir, LS_MODS[m].id, suffix);
}
static esp_err_t stat_job(void *ctx) {
    file_job *j = ctx;
    static const char *const DIRS[] = {MOD_DIR_INTERNAL, MOD_DIR_CARD};
    char path[64];
    struct stat st;
    for (unsigned d = 0; d < 2 && !j->ok; d++) {
        path_of(path, sizeof(path), DIRS[d], j->mod, "");
        j->ok = stat(path, &st) == 0 && (uint32_t)st.st_size == LS_MODS[j->mod].size;
    }
    return ESP_OK;
}
static esp_err_t read_job(void *ctx) {
    file_job *j = ctx;
    static const char *const DIRS[] = {MOD_DIR_INTERNAL, MOD_DIR_CARD};
    char path[64];
    for (unsigned d = 0; d < 2 && !j->ok; d++) {
        path_of(path, sizeof(path), DIRS[d], j->mod, "");
        const int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        uint32_t got = 0;
        while (got < j->size) {
            const int n = read(fd, j->data + got, j->size - got > 8192 ? 8192 : j->size - got);
            if (n <= 0)
                break;
            got += (uint32_t)n;
        }
        uint8_t extra;
        j->ok = got == j->size && read(fd, &extra, 1) <= 0;
        close(fd);
        if (j->ok) {   /* the right length is not the right file: the hash decides */
            uint8_t sha[32];
            j->ok = mbedtls_sha256(j->data, j->size, sha, 0) == 0 && !memcmp(sha, LS_MODS[j->mod].sha, 32);
        }
    }
    return ESP_OK;
}
/* One step of a write: the first (off 0) clears the old file and starts the
   part, each appends up to MOD_WRITE_STEP, the last renames it into place. */
static esp_err_t write_job(void *ctx) {
    file_job *j = ctx;
    char path[64], part[64];
    path_of(path, sizeof(path), MOD_DIR_INTERNAL, j->mod, "");
    path_of(part, sizeof(part), MOD_DIR_INTERNAL, j->mod, ".p");
    j->ok = false;
    if (!j->off) {
        unlink(path);   /* the stale one first: its room is needed */
        unlink(part);
    }
    const int fd = open(part, j->off ? O_WRONLY | O_APPEND : O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return ESP_OK;
    const uint32_t end = j->size - j->off > MOD_WRITE_STEP ? j->off + MOD_WRITE_STEP : j->size;
    uint32_t put = j->off;
    while (put < end) {
        const int n = write(fd, j->data + put, end - put > 4096 ? 4096 : end - put);
        if (n <= 0)
            break;
        put += (uint32_t)n;
    }
    j->ok = close(fd) == 0 && put == end;
    if (j->ok)
        j->off = end;
    if (j->ok && end == j->size)
        j->ok = rename(part, path) == 0;
    if (!j->ok)
        unlink(part);
    return ESP_OK;
}
/* The whole file, step by step. */
static bool write_module(int m, uint8_t *data, uint32_t size) {
    file_job j = {.mod = m, .data = data, .size = size};
    bool more = true;
    while (more)
        more = ls_nvs_run(write_job, &j, 0) == ESP_OK && j.ok && j.off < j.size;
    return j.ok && j.off == j.size;
}

/* /spiffs/apps/modules.ok names the firmware whose modules the storage was
   last checked against. While it is this firmware's, a module of the right
   length is taken as stored; after an update it is not, and each file is
   hashed once (ls_mod_poll, one a second) so a stale one is fetched again
   before anybody opens its app. */
static int8_t s_stamp = -1;     /* -1 not read, 0 other firmware, 1 ours */
static esp_err_t stamp_job(void *ctx) {
    const bool write_it = ctx != NULL;
    char text[sizeof(LS_MOD_STAMP)] = {0};
    const int fd = open(MOD_DIR_INTERNAL "/modules.ok", write_it ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY, 0666);
    if (fd < 0)
        return ESP_FAIL;
    const int n = write_it ? write(fd, LS_MOD_STAMP, sizeof(LS_MOD_STAMP) - 1) : read(fd, text, sizeof(text) - 1);
    close(fd);
    if (write_it)
        return n == (int)sizeof(LS_MOD_STAMP) - 1 ? ESP_OK : ESP_FAIL;
    return !strcmp(text, LS_MOD_STAMP) ? ESP_OK : ESP_FAIL;
}
static void probe(int m) {
    if (s_stamp < 0)
        s_stamp = ls_nvs_run(stamp_job, NULL, 0) == ESP_OK;
    file_job j = {.mod = m, .size = LS_MODS[m].size};
    if (s_stamp)
        ls_nvs_run(stat_job, &j, 0);
    else if ((j.data = heap_caps_malloc(j.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) != NULL) {
        ls_nvs_run(read_job, &j, 0);
        heap_caps_free(j.data);
    }
    mods[m].state = j.ok ? M_STORED : M_ABSENT;
}
static void stamp_when_complete(void) {
    if (s_stamp != 0)
        return;
    for (int m = 0; m < LS_MOD_COUNT; m++)
        if (mods[m].state != M_STORED && mods[m].state != M_LOADED)
            return;
    if (ls_nvs_run(stamp_job, (void *)1, 0) == ESP_OK)
        s_stamp = 1;
}

static bool load_locked(int m);
/* The UI task (a screen) and the console task (a verb) may ask at once. */
static bool load(int m) {
    static atomic_flag busy = ATOMIC_FLAG_INIT;
    if (mods[m].state == M_LOADED)
        return true;
    while (atomic_flag_test_and_set(&busy))
        vTaskDelay(1);
    const bool ok = load_locked(m);
    atomic_flag_clear(&busy);
    return ok;
}
static bool load_locked(int m) {
    if (mods[m].state == M_LOADED)
        return true;
    if (mods[m].state == M_UNKNOWN)
        probe(m);
    if (mods[m].state != M_STORED)
        return false;
    const ls_mod_info_t *info = &LS_MODS[m];
    const int64_t t0 = esp_timer_get_time();
    file_job j = {.mod = m, .size = info->size};
    j.data = heap_caps_malloc(info->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j.data) {
        snprintf(mods[m].why, sizeof(mods[m].why), "no memory");
        mods[m].state = M_FAILED;
        return false;
    }
    ls_nvs_run(read_job, &j, 0);
    if (!j.ok) {   /* wrong bytes under the right name: treat as absent and fetch */
        heap_caps_free(j.data);
        mods[m].state = M_ABSENT;
        return false;
    }
    const int64_t t1 = esp_timer_get_time();
    s_missing[0] = 0;
    /* Every failure says why on the screen, in `mod` and in the log: the
       preflight names a missing symbol or a relocation the loader would skip
       (type, offset, section); the link names the loader error or a link
       memory/timeout problem. */
    char why[sizeof(mods[m].why)] = "";
    const bool linked = ls_lap_preflight_module(j.data, info->size, resolve, why, sizeof(why)) &&
                        ls_lap_link_module(&mods[m].elf, j.data, resolve, why, sizeof(why)) == 0;
    heap_caps_free(j.data);
    const ls_mod_desc_t *d = linked ? ((const ls_mod_desc_t *(*)(void))(uintptr_t)mods[m].elf.entry)() : NULL;
    if (!d || d->magic != LS_MOD_MAGIC) {
        if (linked)
            snprintf(mods[m].why, sizeof(mods[m].why), "no module descriptor (%08lx)",
                     d ? (unsigned long)d->magic : 0ul);
        else if (s_missing[0])
            snprintf(mods[m].why, sizeof(mods[m].why), "needs %s", s_missing);
        else
            snprintf(mods[m].why, sizeof(mods[m].why), "%s", why[0] ? why : "link failed (no reason given)");
        ESP_LOGE("mod", "%s: %s", info->id, mods[m].why);
        if (linked)
            esp_elf_deinit(&mods[m].elf);
        mods[m].state = M_FAILED;
        return false;
    }
    mods[m].why[0] = 0;
    mods[m].desc = d;
    mods[m].state = M_LOADED;
    if (d->init)
        d->init();
    ESP_LOGW("mod", "%s loaded: %u bytes, read %u ms, link %u ms, link stack %u B left", info->id,
             (unsigned)info->size, (unsigned)((t1 - t0) / 1000), (unsigned)((esp_timer_get_time() - t1) / 1000),
             ls_lap_link_spare());
    return true;
}

static int mod_index(const char *id) {
    for (int m = 0; m < LS_MOD_COUNT; m++)
        if (id && !strcmp(id, LS_MODS[m].id))
            return m;
    return -1;
}
bool ls_mod_loaded(const char *id) {
    const int m = mod_index(id);
    return m >= 0 && mods[m].state == M_LOADED;
}
void *ls_mod_sym(const char *id, const char *name, bool do_load) {
    const int m = mod_index(id);
    if (m < 0 || (mods[m].state != M_LOADED && !(do_load && load(m))))
        return NULL;
    const ls_mod_desc_t *d = mods[m].desc;
    for (uint32_t i = 0; i < d->nsyms; i++)
        if (!strcmp(name, d->syms[i].name))
            return d->syms[i].addr;
    return NULL;
}

static const ls_tui_screen_t *real(int i) {
    const ls_mod_screen_info_t *s = &LS_MOD_SCREENS[i];
    const ls_mod_desc_t *d = mods[s->mod].state == M_LOADED ? mods[s->mod].desc : NULL;
    return d && s->ordinal < d->nscreens ? d->screens[s->ordinal] : NULL;
}
/* A module asks where "its" screen is registered: that is the proxy. */
int ls_mod_screen_index_of(const ls_tui_screen_t *screen);
int ls_mod_screen_index_of(const ls_tui_screen_t *screen) {
    for (int i = 0; i < LS_MOD_SCREEN_COUNT; i++)
        if (screen && real(i) == screen)
            return ls_tui_screen_index_of(LS_MOD_SCREENS[i].proxy);
    return ls_tui_screen_index_of(screen);
}

/* ---- fetching a module the storage does not hold (after a firmware update) */
static void fetch_done(int id, ls_fetch_result_t result, void *ctx) {
    const int m = (int)(intptr_t)ctx;
    (void)id;
    bool ok = false;
    if (result == LS_FETCH_CANCELLED && mods[m].bg) {
        /* a background fetch made way for an app the user opened: no failure */
        heap_caps_free(mods[m].fetch_buf);
        mods[m].fetch_buf = NULL;
        mods[m].next_try_ms = now_ms() + MOD_RETRY_MS;
        mods[m].fetching = false;
        return;
    }
    if (result == LS_FETCH_OK) {
        ok = write_module(m, (uint8_t *)mods[m].fetch_buf, LS_MODS[m].size);
        if (!ok)
            snprintf(mods[m].why, sizeof(mods[m].why), "app storage is full or unreadable");
    } else
        snprintf(mods[m].why, sizeof(mods[m].why), "%s",
                 result == LS_FETCH_NO_NET ? "no network" : result == LS_FETCH_HTTP ? "not on the server yet"
                 : result == LS_FETCH_BUSY ? "waiting for the radio or memory" : "download failed");
    heap_caps_free(mods[m].fetch_buf);
    mods[m].fetch_buf = NULL;
    mods[m].fetched_bad = !ok;
    /* one minute, then doubling to an hour: a board that cannot get it stays quiet */
    const uint32_t wait = MOD_RETRY_MS << (mods[m].fails < 6 ? mods[m].fails : 6);
    mods[m].fails = ok ? 0 : (uint8_t)(mods[m].fails + 1);
    mods[m].next_try_ms = now_ms() + (wait > 3600000u ? 3600000u : wait);
    mods[m].state = ok ? M_STORED : M_ABSENT;
    mods[m].fetching = false;
}
/* user: the app was opened, so now; otherwise in the background, like the
   update check: only while no RF app is live and internal RAM is not low. */
static bool fetch(int m, bool user) {
    if (mods[m].fetching || !ls_fetch_online())
        return false;
    const ls_mod_info_t *info = &LS_MODS[m];
    char url[96];
    int n = snprintf(url, sizeof(url), "%s", MOD_URL);
    for (int i = 0; i < 16; i++)
        n += snprintf(url + n, sizeof(url) - (size_t)n, "%02x", info->sha[i]);
    snprintf(url + n, sizeof(url) - (size_t)n, ".lsm");
    mods[m].fetch_buf = heap_caps_malloc(info->size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mods[m].fetch_buf)
        return false;
    const ls_fetch_req_t q = {.url = url, .buf = mods[m].fetch_buf, .cap = info->size + 1, .bytes = info->size,
                              .max_bytes = MOD_MAX_BYTES, .sha256 = info->sha, .timeout_s = 180,
                              .flags = user ? 0 : LS_FETCH_BACKGROUND | LS_FETCH_WAIT_NET,
                              .done = fetch_done, .ctx = (void *)(intptr_t)m};
    mods[m].bg = !user;
    mods[m].fetching = true;
    mods[m].fetch_id = ls_fetch_start(&q);
    if (!mods[m].fetch_id) {
        mods[m].fetching = false;
        heap_caps_free(mods[m].fetch_buf);
        mods[m].fetch_buf = NULL;
        return false;
    }
    return true;
}
void ls_mod_poll(void) {
    static uint32_t next;
    static int turn;
    const uint32_t t = now_ms();
    if ((int32_t)(t - next) < 0 || !LS_MOD_COUNT)
        return;
    next = t + 1000;
    /* One module a second: the first pass looks at what is stored, later
       passes fetch what is not, one download at a time. */
    const int m = turn++ % LS_MOD_COUNT;
    if (m == 0)
        stamp_when_complete();
    if (mods[m].state == M_UNKNOWN)
        probe(m);
    else if (mods[m].state == M_ABSENT && !mods[m].fetching && (int32_t)(t - mods[m].next_try_ms) >= 0) {
        for (int k = 0; k < LS_MOD_COUNT; k++)
            if (mods[k].fetching)
                return;
        fetch(m, false);
    }
}
/* The user opened an app whose module is not stored: background module
   fetches still waiting for a quiet moment step aside (they hold the one
   fetch queue), and this one goes in the foreground once they have. */
static void fetch_for_user(int m) {
    bool waiting = false;
    for (int k = 0; k < LS_MOD_COUNT; k++) {
        ls_fetch_info_t fi;
        if (!mods[k].fetching || !mods[k].bg)
            continue;
        if (ls_fetch_info(mods[k].fetch_id, &fi) && fi.state == LS_FETCH_QUEUED) {
            ls_fetch_cancel(mods[k].fetch_id);
            waiting = true;
        } else
            waiting = waiting || k == m;     /* already on the wire: let it finish */
    }
    if (!waiting && !mods[m].fetching)
        fetch(m, true);
}

/* ---- the proxy screens */
static void say(tui_surface *sf, tui_rect area, int row, const char *text, uint8_t attr) {
    const int n = (int)strlen(text);
    tui_put_str(sf, area, area.x + (area.w > n ? (area.w - n) / 2 : 0), area.y + area.h / 2 + row, text, attr);
}
static void s_enter(int i) {
    screens[i].shown = true;
    screens[i].asked = false;
    /* Opening the app again tries a failed module again (once per visit:
       s_draw stops at M_FAILED), rather than until the next boot. */
    const int m = LS_MOD_SCREENS[i].mod;
    if (mods[m].state == M_FAILED)
        mods[m].state = M_STORED;
    if (mods[m].fetched_bad && mods[m].bg)
        mods[m].fetched_bad = false;   /* a background try that failed: now in the foreground */
    const ls_tui_screen_t *r = real(i);
    if (r) {
        screens[i].entered = true;
        if (r->enter)
            r->enter();
    }
}
static void s_leave(int i) {
    const ls_tui_screen_t *r = real(i);
    if (r && screens[i].entered && r->leave)
        r->leave();
    screens[i].shown = screens[i].entered = false;
}
static void s_draw(int i, tui_surface *sf, tui_rect area) {
    const int m = LS_MOD_SCREENS[i].mod;
    const ls_tui_screen_t *r = real(i);
    if (!r && screens[i].asked && mods[m].state != M_FAILED) {
        /* The frame before this one said LOADING; now do it. */
        if (load(m))
            r = real(i);
        else if (mods[m].state == M_ABSENT && !mods[m].fetched_bad)
            fetch_for_user(m);
    }
    if (r) {
        if (!screens[i].entered) {
            screens[i].entered = true;
            if (r->enter)
                r->enter();
        }
        r->draw(sf, area);
        if (LS_MOD_SCREENS[i].hint && r->hint)
            snprintf(LS_MOD_SCREENS[i].hint, LS_MOD_HINT_MAX, "%s", r->hint);
        return;
    }
    screens[i].asked = true;
    const uint8_t hi = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK), lo = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    char line[64];
    const char *name = LS_MOD_SCREENS[i].proxy->name;
    if (mods[m].state == M_FAILED) {
        snprintf(line, sizeof(line), "%s COULD NOT START", name);
        say(sf, area, -1, line, TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
        say(sf, area, 1, mods[m].why, lo);
    } else if (mods[m].state == M_ABSENT) {
        ls_fetch_info_t fi;
        snprintf(line, sizeof(line), "%s IS NOT ON THIS BOARD YET", name);
        say(sf, area, -2, line, hi);
        if (mods[m].fetching && ls_fetch_info(mods[m].fetch_id, &fi) && fi.total)
            snprintf(line, sizeof(line), "downloading  %u%%", (unsigned)(100u * fi.got / fi.total));
        else if (mods[m].fetching)
            snprintf(line, sizeof(line), "downloading");
        else if (mods[m].fetched_bad)
            snprintf(line, sizeof(line), "%s; trying again shortly", mods[m].why);
        else
            snprintf(line, sizeof(line), "it comes over Wi-Fi after an update: join a network");
        say(sf, area, 0, line, lo);
        say(sf, area, 2, "or put it in /apps on the SD card", lo);
    } else {
        snprintf(line, sizeof(line), "LOADING %s", name);
        say(sf, area, 0, line, hi);
    }
}
static bool s_key(int i, ls_tk_t k, char ch) {
    const ls_tui_screen_t *r = real(i);
    return r && screens[i].entered && r->key ? r->key(k, ch) : false;
}
static bool s_touch(int i, int col, int row) {
    const ls_tui_screen_t *r = real(i);
    return r && screens[i].entered && r->touch ? r->touch(col, row) : false;
}

int ls_mod_command(int argc, char **argv) {
    static const char *const STATE[] = {"unchecked", "stored", "absent", "loaded", "failed"};
    if (argc >= 3 && !strcmp(argv[1], "load")) {
        const int m = mod_index(argv[2]);
        if (m >= 0 && mods[m].state == M_FAILED)
            mods[m].state = M_STORED;   /* an explicit load tries again */
        printf("mod: %s %s\n", argv[2], m < 0 ? "unknown" : load(m) ? "loaded" : mods[m].why[0] ? mods[m].why : "not stored");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "fetch")) {
        for (int m = 0; m < LS_MOD_COUNT; m++) {
            if (mods[m].state == M_UNKNOWN)
                probe(m);
            if (mods[m].state == M_ABSENT) {
                mods[m].next_try_ms = now_ms();
                mods[m].fetched_bad = false;
                mods[m].fails = 0;
            }
        }
        printf("mod: fetching what is missing when the network is up\n");
        return 0;
    }
    for (int m = 0; m < LS_MOD_COUNT; m++) {
        if (mods[m].state == M_UNKNOWN)
            probe(m);
        printf("mod: %-12s %7u bytes  %02x%02x%02x%02x  %s%s %s\n", LS_MODS[m].id, (unsigned)LS_MODS[m].size,
               LS_MODS[m].sha[0], LS_MODS[m].sha[1], LS_MODS[m].sha[2], LS_MODS[m].sha[3], STATE[mods[m].state],
               mods[m].fetching ? " (downloading)" : "", mods[m].why);
    }
    printf("mod: %u firmware symbols exported\n", (unsigned)ls_mod_exports_count);
    return 0;
}

#else

void *ls_mod_sym(const char *id, const char *name, bool load) {
    (void)id; (void)name; (void)load;
    return 0;
}
bool ls_mod_loaded(const char *id) {
    (void)id;
    return false;
}
void ls_mod_poll(void) {}
int ls_mod_command(int argc, char **argv) {
    (void)argc; (void)argv;
    return 0;
}

#endif
