#include "tui/ls_survey.h"
#include "tui/ls_wireless.h"
#include "ls_wifi.h"
#include "ble_link.h"
#include "ls_gps.h"
#include "ls_sdcard.h"
#include "ls_version.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#define fsync _commit
#endif

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
static EXT_RAM_BSS_ATTR ls_survey_session_t s_session;
static ls_survey_view_t s_view;
static ls_survey_fix_t s_fix;
static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static int s_request, s_order_sort = -1;
static bool s_order_dirty = true;
static bool s_ble_owned, s_wifi_owned, s_epoch, s_io_error;
static uint32_t s_start_ms, s_scan_ms, s_base_s, s_flush_ms;
static FILE *s_file, *s_summary;
static char s_summary_path[160];
static EXT_RAM_BSS_ATTR char s_file_buffer[4096], s_summary_buffer[1024];

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
    s_view.wifi = s_session.totals[0]; s_view.ble = s_session.totals[1];
    s_view.cached = s_session.count;
    s_view.wifi_minute = s_view.ble_minute = 0;
    for (unsigned i = 0; i < 60; i++) {
        if (now_ms() / 1000 - s_session.minute_stamp[i] < 60) {
            s_view.wifi_minute += s_session.minute[i][0];
            s_view.ble_minute += s_session.minute[i][1];
        }
    }
    s_view.new_minute = s_view.wifi_minute + s_view.ble_minute;
    memcpy(s_view.security, s_session.security, sizeof(s_view.security));
    memset(s_view.top_channel, 0, sizeof(s_view.top_channel));
    memset(s_view.top_count, 0, sizeof(s_view.top_count));
    for (unsigned ch = 1; ch < 15; ch++) for (unsigned rank = 0; rank < 3; rank++) {
        if (s_session.channels[ch] <= s_view.top_count[rank]) continue;
        for (unsigned j = 2; j > rank; j--) {
            s_view.top_channel[j] = s_view.top_channel[j-1];
            s_view.top_count[j] = s_view.top_count[j-1];
        }
        s_view.top_channel[rank] = ch; s_view.top_count[rank] = s_session.channels[ch]; break;
    }
    s_view.utc_valid = s_gps.utc_s && now_ms() - s_gps.utc_ms < 3000;
    s_view.dropped = s_session.dropped;
    s_view.gps_fresh = ls_survey_fix_fresh(&s_fix, esp_timer_get_time());
    if (s_view.running) s_view.elapsed_s = (now_ms() - s_start_ms) / 1000;
    *out = s_view;
    unlock();
}

static int compare_index(const void *a, const void *b)
{
    return ls_survey_compare(&s_table[*(const uint16_t *)a], &s_table[*(const uint16_t *)b], s_order_sort);
}

bool ls_survey_at(unsigned rank, int sort, ls_survey_entry_t *out)
{
    if (!s_lock) return false;
    lock();
    bool found = rank < s_session.count;
    if (found) {
        if (s_order_dirty || s_order_sort != sort) {
            for (unsigned i = 0; i < s_session.count; i++) s_order[i] = i;
            s_order_sort = sort;
            qsort(s_order, s_session.count, sizeof(s_order[0]), compare_index);
            s_order_dirty = false;
        }
        *out = s_table[s_order[rank]];
    }
    unlock();
    return found;
}

static bool open_session(void);

static void update_fix(void)
{
    s_fix = (ls_survey_fix_t){s_gps.fix, s_gps.lat_deg, s_gps.lon_deg,
        s_gps.last_fix_us, s_gps.alt_m, s_gps.hdop};
    s_epoch = s_gps.utc_s && now_ms() - s_gps.utc_ms < 3000;
    s_base_s = s_gps.utc_s;
    s_view.utc_valid = s_epoch;
}

static void stamp(ls_survey_entry_t *o, int64_t us)
{
    uint32_t ms = (uint32_t)(us / 1000);
    bool utc = s_epoch && ms - s_gps.utc_ms < 3000;
    o->epoch = utc;
    o->last_s = utc ? s_base_s : 0;
    if (s_session.only_fix && !ls_survey_fix_fresh(&s_fix, us)) return;
    ls_survey_note(&s_session, o, &s_fix, us, ms);
    ls_survey_entry_t *e = &s_table[s_session.current];
    if (utc && !s_io_error && ls_survey_relog(e, o, &s_fix, us)) {
        if (!s_file && !open_session()) s_io_error = true;
        ls_survey_fix_t fix = s_fix;
        fix.fix = ls_survey_fix_fresh(&fix, us);
        if (!s_io_error && ls_survey_row(s_file, o, &fix, e->first_s)) {
            ls_survey_logged(e, o, &fix, us); s_view.rows++;
        } else s_io_error = true;
    }
    if (s_io_error || s_session.summary_error)
        snprintf(s_view.status, sizeof(s_view.status), "SD write failed / stop and check card");
    else snprintf(s_view.status, sizeof(s_view.status), "%s",
        utc ? "Passive / accuracy estimate = HDOP x 5 m" : "Waiting for GPS UTC / no rows written");
    s_view.activity_revision++;
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
        int fd = open(s_view.path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd < 0) continue;
        s_file = fdopen(fd, "w");
        if (!s_file) { close(fd); return false; }
        setvbuf(s_file, s_file_buffer, _IOFBF, sizeof(s_file_buffer));
        snprintf(s_summary_path, sizeof(s_summary_path), "%.*s_summary.csv",
            (int)strlen(s_view.path) - 4, s_view.path);
        s_summary = fopen(s_summary_path, "w");
        if (!s_summary) return false;
        setvbuf(s_summary, s_summary_buffer, _IOFBF, sizeof(s_summary_buffer));
        s_session.summary = s_summary;
        ls_version_info_t version; ls_version_get(&version);
        bool ok = ls_survey_header(s_file, version.version, version.board);
        fputs("MAC,Type,AddressType,SSID,Samples,EstimatedLatitude,EstimatedLongitude,Weight\n", s_summary);
        s_flush_ms = now_ms();
        return ok && !ferror(s_summary);
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
        char summary[160];
        snprintf(summary, sizeof(summary), "%.*s_summary.csv", (int)strlen(oldest) - 4, oldest);
        unlink(summary);
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
    lock();
    ls_gps_get(&s_gps);
    update_fix();
    int request = s_request;
    s_request = 0;
    ls_survey_options_t options = s_view.options;
    unlock();
    if (request == 1) {
        s_start_ms = now_ms();
        lock();
        ls_survey_init(&s_session, s_table, LS_SURVEY_CAP, options.only_fix);
        s_view.rows = 0; s_view.path[0] = 0; s_io_error = false;
        s_flush_ms = s_start_ms;
        unlock();
        /* The first thing that fails is what the operator is told. */
        const char *why = NULL;
        bool ok = true;
        if (options.wifi && (ls_wifi_sta_running() || ls_wifi_running())) { ok = false; why = "Wi-Fi is on: turn STA and AP off first"; }
        else if (options.ble && ble_link_state() != BLE_LINK_OFF) { ok = false; why = "BLE link is on: turn it off first"; }
        if (ok && options.wifi) { ok = ls_wifi_survey_mode(true) == ESP_OK; s_wifi_owned = ok; if (!ok) why = "Wi-Fi radio would not enter survey"; }

        if (ok && !ls_gps_running()) {
            esp_err_t rc = ls_gps_start();
            if (options.only_fix && rc != ESP_OK) { ok = false; why = "GPS would not start (fix-only is on)"; }
        }
        if (ok && options.ble) { ok = ble_link_listen() == ESP_OK; s_ble_owned = ok; if (!ok) why = "BLE scan would not start"; }
        if (!ok && s_wifi_owned) { ls_wifi_survey_mode(false); s_wifi_owned = false; }
        lock();
        if (ok) {
            s_order_dirty = true;
            s_scan_ms = s_start_ms - options.interval_s * 1000;
            s_view.elapsed_s = 0;
            snprintf(s_view.status, sizeof(s_view.status), "Waiting for GPS UTC / no rows written");
        } else {
            if (s_file) { fclose(s_file); s_file = NULL; }
            snprintf(s_view.status, sizeof(s_view.status), "%s", why ? why : "Cannot start");
        }
        s_view.running = ok; s_view.busy = false;
        unlock();
    }
    if (request == -1) {
        lock(); s_view.running = false; unlock();
        if (s_ble_owned) { ble_link_stop(); s_ble_owned = false; }
        if (s_wifi_owned) { ls_wifi_survey_mode(false); s_wifi_owned = false; }
        lock();
        bool had_file = s_file != NULL;
        bool ok = !s_io_error && !s_session.summary_error;
        if (s_summary) for (unsigned i = 0; i < s_session.count; i++)
            if (!ls_survey_summary(s_summary, &s_table[i])) ok = false;
        if (s_file) {
            if (fflush(s_file) || fsync(fileno(s_file))) ok = false;
            if (fclose(s_file)) ok = false;
            s_file = NULL;
        }
        if (s_summary) {
            if (fflush(s_summary) || fsync(fileno(s_summary))) ok = false;
            if (fclose(s_summary)) ok = false;
            s_summary = NULL; s_session.summary = NULL;
        }
        bool retained = ok && had_file && retain(options.keep);
        snprintf(s_view.status, sizeof(s_view.status), "%s", !ok ? "SD export failed / existing rows retained" :
            !had_file ? "No GPS UTC observations / no file written" :
            retained ? "Session saved / centroid segments include evictions" : "Session saved / retention cleanup failed");
        s_view.busy = false;
        unlock();
    }
    lock();
    bool running = s_view.running;
    if (running && s_epoch && !s_file && !s_io_error) {
        s_io_error = !open_session();
        snprintf(s_view.status, sizeof(s_view.status), "%s", s_io_error ?
            "SD open failed / stop and check card" : "Passive / accuracy estimate = HDOP x 5 m");
    }
    if (running && !s_epoch && !s_io_error)
        snprintf(s_view.status, sizeof(s_view.status), "Waiting for GPS UTC / no rows written");
    unlock();
    if (running && now_ms() - s_flush_ms >= 5000) {
        lock();
        s_flush_ms = now_ms();
        if (s_file && (fflush(s_file) || fsync(fileno(s_file)))) s_io_error = true;
        if (s_summary && (fflush(s_summary) || fsync(fileno(s_summary)))) s_io_error = true;
        uint64_t total;
        s_view.sd_known = ls_sdcard_size(&total, &s_view.free_bytes);
        if (s_io_error) snprintf(s_view.status, sizeof(s_view.status), "SD write failed / stop and check card");
        unlock();
    }
    if (running && options.wifi && now_ms() - s_scan_ms >= options.interval_s * 1000) {
        s_scan_ms = now_ms();
        int n = ls_wifi_survey_scan(s_scan, 64);
        lock();
        ls_gps_get(&s_gps);
        update_fix();
        if (n >= 0) s_view.activity_revision++;
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
