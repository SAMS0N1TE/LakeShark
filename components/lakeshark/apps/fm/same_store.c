#include "same_store.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include <stdio.h>
#include <sys/stat.h>
#include "nvs.h"
#include "ls_nvs_safe.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
static EXT_RAM_BSS_ATTR same_alert_t history[SAME_HISTORY];
static same_options_t options;
static int head, count, pending = -1;
#ifdef ESP_PLATFORM
/* Keep snapshots coherent without letting a higher-priority reader starve RX. */
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static void lock(void) { portENTER_CRITICAL(&guard); }
static void unlock(void) { portEXIT_CRITICAL(&guard); }
#else
static unsigned guard;
static void lock(void) { while (__atomic_exchange_n(&guard, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&guard, 0, __ATOMIC_RELEASE); }
#endif
#ifdef ESP_PLATFORM
typedef struct { same_options_t value; bool save; } option_job_t;
static esp_err_t option_job(void *arg)
{
    option_job_t *j = arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open("same", j->save ? NVS_READWRITE : NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    /* Fixed-width fields keep preferences independent of C struct padding. */
    uint32_t data[7]; size_t bytes = sizeof(data);
    if (j->save) {
        memcpy(data, j->value.fips, sizeof(j->value.fips));
        data[6] = j->value.only_mine | (j->value.include_tests << 1) | (j->value.log_sd << 2);
        err = nvs_set_blob(h, "options_v1", data, bytes);
        if (err == ESP_OK) err = nvs_commit(h);
    } else {
        err = nvs_get_blob(h, "options_v1", data, &bytes);
        if (err == ESP_OK && bytes == sizeof(data)) {
            for (int i = 0; i < 6; ++i) j->value.fips[i] = data[i] <= 99999 ? data[i] : 0;
            j->value.only_mine = (data[6] & 1) != 0;
            j->value.include_tests = (data[6] & 2) != 0;
            j->value.log_sd = (data[6] & 4) != 0;
        } else err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(h); return err;
}
#endif
void same_options_load(void)
{
#ifdef ESP_PLATFORM
    option_job_t j = {0};
    if (ls_nvs_run(option_job, &j, 0) == ESP_OK) {
        lock(); options = j.value; unlock();
    }
#endif
}
void same_options_get(same_options_t *out) { lock(); *out = options; unlock(); }
void same_options_set(const same_options_t *o)
{
    lock();
    bool changed = memcmp(options.fips, o->fips, sizeof(options.fips)) ||
        options.only_mine != o->only_mine || options.include_tests != o->include_tests || options.log_sd != o->log_sd;
    options = *o; unlock();
#ifdef ESP_PLATFORM
    if (changed) {
        option_job_t j = { .value = *o, .save = true };
        ls_nvs_run(option_job, &j, 0);
    }
#else
    (void)changed;
#endif
}
int same_store_count(void) { lock(); int n = count; unlock(); return n; }
bool same_store_get(int index, same_alert_t *out)
{
    lock(); bool ok = index >= 0 && index < count;
    if (ok) *out = history[(head + SAME_HISTORY - 1 - index) % SAME_HISTORY];
    unlock(); return ok;
}
bool same_store_notice(same_alert_t *out)
{
    lock(); bool ok = pending >= 0;
    if (ok) { *out = history[pending]; pending = -1; }
    unlock(); return ok;
}
void same_store_receive(const same_alert_t *a, void *user)
{
    (void)user;
    lock();
    if (a->ended) {
        for (int i = 0; i < count; ++i) {
            int k = (head + SAME_HISTORY - 1 - i) % SAME_HISTORY;
            if (!strcmp(history[k].header, a->header)) { history[k].ended = true; break; }
        }
        unlock(); return;
    }
    history[head] = *a;
    if (same_matches(a, &options)) pending = head;
    head = (head + 1) % SAME_HISTORY; if (count < SAME_HISTORY) ++count;
    bool log = options.log_sd; unlock();
#ifdef ESP_PLATFORM
    /* SD failures leave reception and notifications running. One line per voted alert. */
    if (log) {
        mkdir("/sdcard/lakeshark", 0775);
        mkdir("/sdcard/lakeshark/alerts", 0775);
        FILE *f = fopen("/sdcard/lakeshark/alerts/same.log", "a");
        if (f) { fprintf(f, "%s\n", a->header); fclose(f); }
    }
#else
    (void)log;
#endif
}
