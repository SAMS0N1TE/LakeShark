#include "tui/ls_survey.h"
#include "tui/ls_wireless.h"
#include "ls_wifi.h"
#include "ble_link.h"
#include "ls_gps.h"
#include "ls_track.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef LS_SURVEY_DIRECTORY
#define LS_SURVEY_DIRECTORY "/sdcard/lakeshark/survey"
#define LS_SURVEY_DEFAULT_DIR
#endif
#define SURVEY_DIR LS_SURVEY_DIRECTORY
static void make_dir(const char *path)
{
#ifdef _WIN32
    mkdir(path);
#else
    mkdir(path, 0777);
#endif
}
static EXT_RAM_BSS_ATTR ls_survey_entry_t s_table[LS_SURVEY_CAP];
static EXT_RAM_BSS_ATTR ls_wifi_scan_ap_t s_scan[64];
static EXT_RAM_BSS_ATTR ls_gps_state_t s_gps;
static EXT_RAM_BSS_ATTR uint16_t s_order[LS_SURVEY_CAP];
static ls_survey_session_t s_session;
static ls_survey_view_t s_view;
static ls_survey_fix_t s_fix;
static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static int s_request, s_order_sort = -1;
static bool s_order_dirty = true;
static bool s_ble_owned, s_wifi_owned, s_epoch;
static uint32_t s_start_ms, s_scan_ms, s_base_s;
static FILE *s_file;
static char s_partial[144];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

bool ls_survey_run(bool start, const ls_survey_options_t *o)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    lock();
    bool accept = !s_view.busy && !s_request && start != s_view.running;
    if (start && (!o || (!o->wifi && !o->ble) || o->interval_s < 3 ||
                  o->interval_s > 120 || o->keep < 1 || o->keep > 50)) accept = false;
    if (accept) {
        if (start) s_view.options = *o;
        s_request = start ? 1 : -1;
        s_view.busy = true;
        if (!start) { s_view.running = false; s_view.elapsed_s = (now_ms() - s_start_ms) / 1000; }
    }
    unlock();
    if (accept) ls_wireless_set_active(true);
    return accept;
}

void ls_survey_view(ls_survey_view_t *out)
{
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    lock();
    s_view.wifi = s_view.ble = s_view.new_minute = 0;
    for (unsigned i = 0; i < s_session.count; i++) {
        const ls_survey_entry_t *e = &s_table[i];
        if (e->kind == LS_SURVEY_WIFI) s_view.wifi++; else s_view.ble++;
        if (now_ms() - e->first_ms < 60000) s_view.new_minute++;
    }
    s_view.dropped = s_session.dropped;
    s_view.gps_fresh = ls_survey_fix_fresh(&s_fix, esp_timer_get_time());
    if (s_view.running) s_view.elapsed_s = (now_ms() - s_start_ms) / 1000;
    *out = s_view;
    unlock();
}

bool ls_survey_at(unsigned rank, int sort, ls_survey_entry_t *out)
{
    if (!s_lock) return false;
    lock();
    bool found = rank < s_session.count;
    if (found) {
        if (s_order_dirty || s_order_sort != sort) {
            /* A small insertion sort of indices leaves observation storage stable. */
            for (unsigned i = 0; i < s_session.count; i++) {
                unsigned j = i;
                while (j && ls_survey_compare(&s_table[i], &s_table[s_order[j - 1]], sort) < 0) {
                    s_order[j] = s_order[j - 1]; j--;
                }
                s_order[j] = i;
            }
            s_order_sort = sort; s_order_dirty = false;
        }
        *out = s_table[s_order[rank]];
    }
    unlock();
    return found;
}

static void stamp(ls_survey_entry_t *o, int64_t us)
{
    uint32_t ms = (uint32_t)(us / 1000);
    o->epoch = s_epoch;
    o->last_s = s_base_s + (ms - s_start_ms) / 1000;
    ls_survey_note(&s_session, o, &s_fix, us, ms);
    s_order_dirty = true;
}

void ls_survey_ble(const uint8_t *addr, uint8_t type, const char *name,
                    unsigned len, int rssi, int64_t us)
{
    if (!s_lock) return;
    lock();
    if (s_view.running && s_view.options.ble) {
        ls_survey_entry_t o = {.kind = LS_SURVEY_BLE, .addr_type = type, .rssi = rssi};
        /* NimBLE stores the address least significant octet first. */
        for (int i = 0; i < 6; i++) o.addr[i] = addr[5 - i];
        if (name) { if (len > 32) len = 32; memcpy(o.name, name, len); }
        stamp(&o, us);
    }
    unlock();
}

static bool open_session(void)
{
#ifdef LS_SURVEY_DEFAULT_DIR
    make_dir("/sdcard/lakeshark");
#endif
    make_dir(SURVEY_DIR);
    char name[48], base[48];
    if (!ls_survey_filename(base, sizeof(base), s_base_s, s_epoch)) return false;
    for (int i = 0; i < 1000; i++) {
        if (i) snprintf(name, sizeof(name), "%.*s_%03d.csv", (int)strlen(base) - 4, base, i);
        else snprintf(name, sizeof(name), "%s", base);
        snprintf(s_view.path, sizeof(s_view.path), "%s/%s", SURVEY_DIR, name);
        struct stat st;
        if (!stat(s_view.path, &st)) continue;
        snprintf(s_partial, sizeof(s_partial), "%s.partial", s_view.path);
        int fd = open(s_partial, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd < 0) continue;
        s_file = fdopen(fd, "w");
        if (!s_file) { close(fd); unlink(s_partial); return false; }
        return true;
    }
    return false;
}

/* Retention only considers files whose names this recorder generates. */
static bool session_name(const char *name)
{
    const char *p = name;
    if (!strncmp(p, "undated_", 8)) {
        p += 8;
        for (int i = 0; i < 10; i++, p++) if (*p < '0' || *p > '9') return false;
    } else {
        for (int i = 0; i < 15; i++, p++) {
            if (i == 8) { if (*p != '_') return false; }
            else if (*p < '0' || *p > '9') return false;
        }
    }
    if (*p == '_') { p++; for (int i = 0; i < 3; i++, p++) if (*p < '0' || *p > '9') return false; }
    return !strcmp(p, ".csv");
}

static bool retain(unsigned keep)
{
    for (;;) {
        DIR *dir = opendir(SURVEY_DIR);
        if (!dir) return false;
        unsigned count = 0;
        char oldest[144] = "", path[144];
        time_t oldest_time = 0;
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!session_name(entry->d_name)) continue;
            if (strlen(SURVEY_DIR) + 1 + strlen(entry->d_name) >= sizeof(path)) continue;
            strcpy(path, SURVEY_DIR); strcat(path, "/"); strcat(path, entry->d_name);
            struct stat st;
            if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
            count++;
            if (!strcmp(path, s_view.path)) continue;
            if (!oldest[0] || st.st_mtime < oldest_time ||
                (st.st_mtime == oldest_time && strcmp(path, oldest) < 0)) {
                snprintf(oldest, sizeof(oldest), "%s", path); oldest_time = st.st_mtime;
            }
        }
        closedir(dir);
        if (count <= keep) return true;
        if (!oldest[0] || unlink(oldest)) return false;
    }
}

bool ls_survey_pending(void)
{
    if (!s_lock) return false;
    lock(); bool active = s_view.running || s_request || s_view.busy; unlock();
    return active;
}

void ls_survey_tick(void)
{
    if (!s_lock) return;
    ls_gps_get(&s_gps);
    lock();
    s_fix = (ls_survey_fix_t){s_gps.fix, s_gps.lat_deg, s_gps.lon_deg, s_gps.last_fix_us};
    int request = s_request;
    s_request = 0;
    ls_survey_options_t options = s_view.options;
    unlock();
    if (request == 1) {
        uint8_t flags = 0;
        s_start_ms = now_ms();
        s_base_s = ls_track_time(s_gps.year, s_gps.month, s_gps.day,
            s_gps.hour, s_gps.minute, s_gps.second, s_start_ms / 1000, &flags);
        s_epoch = (flags & LS_TRACK_F_EPOCH) && s_gps.last_sentence_us > 0 &&
            esp_timer_get_time() >= s_gps.last_sentence_us &&
            esp_timer_get_time() - s_gps.last_sentence_us <= 10000000;
        if (!s_epoch) s_base_s = s_start_ms / 1000;
        /* The first thing that fails is what the operator is told. */
        const char *why = NULL;
        bool ok = true;
        if (options.wifi && (ls_wifi_sta_running() || ls_wifi_running())) { ok = false; why = "Wi-Fi is on: turn STA and AP off first"; }
        else if (options.ble && ble_link_state() != BLE_LINK_OFF) { ok = false; why = "BLE link is on: turn it off first"; }
        if (ok && options.wifi) { ok = ls_wifi_survey_mode(true) == ESP_OK; s_wifi_owned = ok; if (!ok) why = "Wi-Fi radio would not enter survey"; }
        if (ok) { lock(); ok = open_session(); unlock(); if (!ok) why = "SD card not writable"; }
        if (ok && !ls_gps_running()) {
            esp_err_t rc = ls_gps_start();
            if (options.only_fix && rc != ESP_OK) { ok = false; why = "GPS would not start (fix-only is on)"; }
        }
        if (ok && options.ble) { ok = ble_link_listen() == ESP_OK; s_ble_owned = ok; if (!ok) why = "BLE scan would not start"; }
        if (!ok && s_wifi_owned) { ls_wifi_survey_mode(false); s_wifi_owned = false; }
        lock();
        if (ok) {
            ls_survey_init(&s_session, s_table, LS_SURVEY_CAP, options.only_fix);
            s_order_dirty = true;
            s_scan_ms = s_start_ms - options.interval_s * 1000;
            s_view.elapsed_s = 0;
            snprintf(s_view.status, sizeof(s_view.status), "Passive survey / positions require a fresh fix");
        } else {
            if (s_file) { fclose(s_file); s_file = NULL; unlink(s_partial); }
            snprintf(s_view.status, sizeof(s_view.status), "%s", why ? why : "Cannot start");
        }
        s_view.running = ok; s_view.busy = false;
        unlock();
    }
    if (request == -1) {
        lock(); s_view.running = false; unlock();
        if (s_ble_owned) { ble_link_stop(); s_ble_owned = false; }
        if (s_wifi_owned) { ls_wifi_survey_mode(false); s_wifi_owned = false; }
        bool ok = s_file && ls_survey_csv(s_file, &s_session);
        if (s_file) { if (fclose(s_file)) ok = false; s_file = NULL; }
        if (ok) ok = rename(s_partial, s_view.path) == 0;
        bool retained = ok && retain(options.keep);
        lock();
        snprintf(s_view.status, sizeof(s_view.status), "%s", ok ? (retained ? "Session saved / random BLE addresses rotate" : "Session saved / retention cleanup failed") : "Export failed / partial file retained; table still available");
        s_view.busy = false;
        unlock();
    }
    lock(); bool running = s_view.running; unlock();
    if (running && options.wifi && now_ms() - s_scan_ms >= options.interval_s * 1000) {
        s_scan_ms = now_ms();
        int n = ls_wifi_survey_scan(s_scan, 64);
        ls_gps_get(&s_gps);
        lock();
        s_fix = (ls_survey_fix_t){s_gps.fix, s_gps.lat_deg, s_gps.lon_deg, s_gps.last_fix_us};
        if (n < 0) snprintf(s_view.status, sizeof(s_view.status), "Passive Wi-Fi scan unavailable / radio busy");
        for (int i = 0; s_view.running && i < n; i++) {
            ls_survey_entry_t o = {.kind = LS_SURVEY_WIFI, .rssi = s_scan[i].rssi,
                                  .channel = s_scan[i].channel, .auth = s_scan[i].auth};
            memcpy(o.addr, s_scan[i].bssid, 6); memcpy(o.name, s_scan[i].ssid, 33);
            stamp(&o, esp_timer_get_time());
        }
        unlock();
    }
}
