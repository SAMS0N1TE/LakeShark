/* See p25_sites.h. SD and NVS never run on the UI/decoder task. */
#include "p25_sites.h"
#include "p25_program.h"
#include "p25_controls.h"
#include "p25_geo.h"
#include "ls_gps.h"
#include "ls_nvs_safe.h"
#include "nvs.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "ls_textpack.h"

static QueueHandle_t jobs;
static atomic_bool enabled = true;
static _Atomic(p25_pack_db_t *) pending;
static p25_pack_db_t *active; /* owner only */
static p25_geo_t geo;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR p25_sites_status_t status;
static const ls_radio_range_t ranges[] = {{P25_CONTROL_TUNER_MIN_HZ, P25_CONTROL_TUNER_MAX_HZ}};
static const p25_profile_parse_config_t config = {ranges, 1};

typedef struct { int op; uint8_t value; } job_t;
static esp_err_t preference(void *ctx)
{
    job_t *j = ctx; nvs_handle_t n;
    esp_err_t e = nvs_open("p25packs", NVS_READWRITE, &n);
    if (e != ESP_OK) return e;
    if (j->op == 0) {
        uint8_t v = 1;
        e = nvs_get_u8(n, "gps_sites", &v);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) atomic_store(&enabled, v != 0);
    } else {
        e = nvs_set_u8(n, "gps_sites", j->value);
        if (e == ESP_OK) e = nvs_commit(n);
        if (e == ESP_OK) atomic_store(&enabled, j->value != 0);
    }
    nvs_close(n); return e;
}
static void error(const char *s)
{
    portENTER_CRITICAL(&status_lock);
    snprintf(status.error, sizeof(status.error), "%s", s);
    portEXIT_CRITICAL(&status_lock);
}
static void load(void)
{
    p25_pack_db_t *db = heap_caps_calloc(1, sizeof(*db), MALLOC_CAP_SPIRAM);
    char *buf = heap_caps_malloc(P25_PACK_MAX_BYTES + 1, MALLOC_CAP_SPIRAM);
    p25_profile_parse_scratch_t *scratch = heap_caps_malloc(sizeof(*scratch), MALLOC_CAP_SPIRAM);
    if (!db || !buf || !scratch) { error(LS_TEXT(t_p25_sites_no_psram_for_site, "No PSRAM for site packs")); goto done; }
    error("");
    DIR *d = opendir("/sdcard/p25/sites");
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d))) {
            if (strlen(entry->d_name) == 10 && !strcmp(entry->d_name + 2, ".txt.bak")) {
                char st[3] = {entry->d_name[0], entry->d_name[1], 0};
                if (p25_pack_state_valid(st)) {
                    char final[64], backup[80];
                    LS_SNPRINTF(t_p25_sites_sdcard_p25_sites_s, final, sizeof(final), "/sdcard/p25/sites/%s.txt", st);
                    snprintf(backup, sizeof(backup), "%s.bak", final);
                    FILE *present = fopen(final, "rb");
                    if (present) fclose(present);
                    else (void)rename(backup, final);
                }
            }
        }
        rewinddir(d);
        while ((entry = readdir(d))) {
            if (strlen(entry->d_name) != 6 || strcmp(entry->d_name + 2, ".txt")) continue;
            char state[3] = {entry->d_name[0], entry->d_name[1], 0};
            if (!p25_pack_state_valid(state)) continue;
            char path[64]; LS_SNPRINTF(t_p25_sites_sdcard_p25_sites_s_2, path, sizeof(path), "/sdcard/p25/sites/%s", entry->d_name);
            FILE *f = fopen(path, "rb");
            if (!f) { error(LS_TEXT(t_p25_sites_cannot_read_a_site, "Cannot read a site pack")); continue; }
            size_t len = fread(buf, 1, P25_PACK_MAX_BYTES + 1, f);
            bool bad = ferror(f); fclose(f);
            p25_profile_diagnostic_t diag;
            if (bad || !p25_pack_parse(db, scratch, buf, len, &config, &diag))
                error(LS_TEXT(t_p25_sites_pack_invalid_or_capacity, "Pack invalid or capacity exceeded; pack skipped"));
        }
        closedir(d);
    }
    p25_pack_db_t *old = atomic_exchange(&pending, db);
    heap_caps_free(old); db = NULL;
done:
    heap_caps_free(db); heap_caps_free(buf); heap_caps_free(scratch);
}
static void worker(void *arg)
{
    (void)arg;
    job_t j = {0};
    if (ls_nvs_run(preference, &j, 0) != ESP_OK) error(LS_TEXT(t_p25_sites_gps_preference_could_not, "GPS preference could not be read"));
    load();
    for (;;) {
        if (xQueueReceive(jobs, &j, portMAX_DELAY) != pdTRUE) continue;
        if (j.op == 1) {
            if (ls_nvs_run(preference, &j, 0) != ESP_OK) error(LS_TEXT(t_p25_sites_gps_preference_could_not_2, "GPS preference could not be saved"));
        } else load();
    }
}
void p25_sites_init(void)
{
    if (jobs) return;
    strcpy(status.state, "no packs");
    status.distance_km = -1;
    jobs = xQueueCreate(4, sizeof(job_t));
    if (!jobs) { error(LS_TEXT(t_p25_sites_no_site_worker_queue, "No site worker queue")); return; }
    if (xTaskCreatePinnedToCoreWithCaps(worker, "p25_sites", 4096, NULL, 3, NULL,
                                           tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(jobs); jobs = NULL; error("No site worker task");
    }
}
bool p25_sites_reload(void)
{ job_t j = {.op = 2}; return jobs && xQueueSend(jobs, &j, 0) == pdTRUE; }
bool p25_sites_set(bool value)
{ job_t j = {1, value}; return jobs && xQueueSend(jobs, &j, 0) == pdTRUE; }
static void resume_owner(uint32_t arg)
{
    (void)arg; p25_program_geo_resume_now();
    memset(&geo, 0, sizeof(geo)); geo.chosen = -1;
}
bool p25_sites_resume(void) { return p25_owner_post(resume_owner, 0); }
void p25_sites_status(p25_sites_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&status_lock); *out = status; portEXIT_CRITICAL(&status_lock);
    out->enabled = atomic_load(&enabled);
}
int p25_sites_poll(bool may_tune)
{
    p25_pack_db_t *next = atomic_exchange(&pending, NULL);
    if (next) {
        heap_caps_free(active); active = next;
        memset(&geo, 0, sizeof(geo)); geo.chosen = -1;
    }
    static EXT_RAM_BSS_ATTR ls_gps_state_t gps;
    ls_gps_get(&gps);
    const p25_program_t *p = p25_program_session();
    bool paused = !atomic_load(&enabled) || (p && p->geo_paused);
    bool fix = ls_gps_running() && gps.fix && gps.last_fix_us > 0 &&
               gps.last_fix_us <= esp_timer_get_time() &&
               esp_timer_get_time() - gps.last_fix_us <= P25_GEO_FIX_MAX_AGE_US;
    int index = -1;
    if (!paused && may_tune && active && active->count &&
        !p25_program_survey_active_now() && (!p || p->state != P25_PROGRAM_BUSY)) {
        /* Work on a copy: a busy PROGRAM must not consume a geo decision. */
        p25_geo_t candidate = geo;
        index = p25_geo_select_many(&candidate, active->records, active->count,
                                    fix, gps.lat_deg, gps.lon_deg, gps.last_fix_us, esp_timer_get_time());
        if (index < 0 || p25_program_site_now(&active->records[index / P25_PROFILE_CONTROL_MAX],
                                             index % P25_PROFILE_CONTROL_MAX)) geo = candidate;
        else index = -1;
    }
    portENTER_CRITICAL(&status_lock);
    status.packs = active ? active->packs : 0;
    status.sites = active ? active->sites : 0;
    snprintf(status.state, sizeof(status.state), "%s", paused ? "paused" :
             !status.packs ? "no packs" : !fix ? "no fix" : "following");
    status.site[0] = status.system[0] = 0; status.distance_km = -1;
    if (active && geo.have_answer && geo.chosen >= 0) {
        const p25_profile_t *record = &active->records[geo.chosen / P25_PROFILE_CONTROL_MAX];
        snprintf(status.site, sizeof(status.site), "%s", record->site_name);
        snprintf(status.system, sizeof(status.system), "%s", record->system_name);
        status.distance_km = geo.distance_m / 1000.0f;
    }
    portEXIT_CRITICAL(&status_lock);
    return index;
}
