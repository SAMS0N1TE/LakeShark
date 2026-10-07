#include "settings.h"
#include "settings_schema.h"
#include "home_widget_pref.h"
#include "location_pref.h"
#include "radio_choice.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "p25_controls.h"
#include "p25_demod_pref_cache.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* For the auto-dim default below, which is a board capability. */
#include "ls_board.h"
#include "ls_nvs_safe.h"

static const char  *TAG      = "settings";
static const char  *NS       = "sdr-tool";
static nvs_handle_t s_nvs    = 0;
static bool         s_nvs_ok = false;
static bool         s_home_write_ready = false;
/* Read every UI frame and by the console (whose stack may be in TCM).
 * Load on the cache-safe boot task; never issue flash reads in that path. */
static bool         s_auto_rotate = true;
/* Radios that come up at boot. BLE scans continuously once started and is
   the one that costs power with nothing attached, so it is separately
   switchable from the Wi-Fi that shares the same co-processor. */
static bool         s_ble_at_boot = true;
static bool         s_wifi_at_boot = true;
static bool         s_keyboard_light = true;
/* Settings > DEVICE > Check for updates: Daily (true) or Off. Read by the update
   scheduler's timer, so it is RAM only; loaded once in settings_init. */
static bool         s_update_check = true;
static bool         s_lr_tcxo = true, s_lr_dcdc;
static uint64_t s_location;
static uint64_t s_last_fix;   /* COMPASS declination without a live fix */
static portMUX_TYPE s_location_lock = portMUX_INITIALIZER_UNLOCKED;
/* The radio each job runs on, four bits a job, 0xF for none. Read by the
   receive tasks, some with PSRAM stacks, so it is loaded once at boot and
   every read after that is this word. */
static uint64_t s_radio_choice = LS_RSEL_PACKED_NONE;
static portMUX_TYPE s_radio_choice_lock = portMUX_INITIALIZER_UNLOCKED;

#define SET_Q_DEPTH     16
#define SET_PENDING_MAX 16
#define SET_QUIET_MS    300
/* 4096 with 3172 unused - about 924 B in use. */
#define SET_STACK_WORDS (2560 / sizeof(StackType_t))

typedef enum { SV_U8, SV_I8, SV_U32, SV_I32, SV_U64 } sv_type_t;

typedef struct {
    char      key[NVS_KEY_NAME_MAX_SIZE];
    sv_type_t type;
    union { uint8_t u8; int8_t i8; uint32_t u32; int32_t i32; uint64_t u64; } v;
} set_write_t;

static QueueHandle_t   s_wq = NULL;
static StaticQueue_t   s_wq_ctrl;
static uint8_t         s_wq_store[SET_Q_DEPTH * sizeof(set_write_t)];
static StackType_t     s_worker_stack[SET_STACK_WORDS];
static StaticTask_t    s_worker_tcb;
static uint32_t        s_writes_done = 0, s_writes_dropped = 0, s_commits = 0;

static esp_err_t nvs_apply(const set_write_t *w)
{
    switch (w->type) {
    case SV_U8:  return nvs_set_u8 (s_nvs, w->key, w->v.u8);
    case SV_I8:  return nvs_set_i8 (s_nvs, w->key, w->v.i8);
    case SV_U32: return nvs_set_u32(s_nvs, w->key, w->v.u32);
    case SV_I32: return nvs_set_i32(s_nvs, w->key, w->v.i32);
    case SV_U64: return nvs_set_u64(s_nvs, w->key, w->v.u64);
    }
    return ESP_ERR_INVALID_ARG;
}

static void set_worker(void *arg)
{
    (void)arg;
    set_write_t pending[SET_PENDING_MAX];
    int n_pending = 0;
    set_write_t w;

    for (;;) {

        if (xQueueReceive(s_wq, &w, portMAX_DELAY) != pdTRUE) continue;

        n_pending = 0;
        pending[n_pending++] = w;

        while (xQueueReceive(s_wq, &w, pdMS_TO_TICKS(SET_QUIET_MS)) == pdTRUE) {
            int found = -1;
            for (int i = 0; i < n_pending; i++) {
                if (!strcmp(pending[i].key, w.key)) { found = i; break; }
            }
            if (found >= 0) {
                pending[found] = w;
            } else if (n_pending < SET_PENDING_MAX) {
                pending[n_pending++] = w;
            } else {

                esp_err_t err = ESP_OK;
                for (int i = 0; i < n_pending; i++) {
                    esp_err_t one = nvs_apply(&pending[i]);
                    if (err == ESP_OK && one != ESP_OK) err = one;
                }
                if (err == ESP_OK) err = nvs_commit(s_nvs);
                if (err != ESP_OK) ESP_LOGW(TAG, "settings write failed: %d", err);
                s_writes_done += n_pending;
                s_commits++;
                n_pending = 0;
                pending[n_pending++] = w;
            }
        }

        esp_err_t err = ESP_OK;
        for (int i = 0; i < n_pending; i++) {
            esp_err_t one = nvs_apply(&pending[i]);
            if (err == ESP_OK && one != ESP_OK) err = one;
        }
        if (err == ESP_OK) err = nvs_commit(s_nvs);
        if (err != ESP_OK) ESP_LOGW(TAG, "settings write failed: %d", err);
        s_writes_done += n_pending;
        s_commits++;
    }
}

/* Every flash read, like every write, must run on a DRAM stack: callers
 * here include PSRAM-stack tasks (scan, REC, ADS-B, audio) and TCM-stack ones
 * (console). On a DRAM stack this is the plain call; elsewhere it is handed to
 * the ls_nvs worker, and a failed hand-off returns an error (getters then give
 * their default) instead of asserting with the cache off. */
typedef enum { SG_U8, SG_I8, SG_U32, SG_I32, SG_U64 } sg_type_t;
typedef struct { sg_type_t t; const char *key; void *out; } sget_job_t;

static esp_err_t sget_job(void *ctx)
{
    sget_job_t *j = (sget_job_t *)ctx;
    switch (j->t) {
    case SG_U8:  return nvs_get_u8 (s_nvs, j->key, (uint8_t  *)j->out);
    case SG_I8:  return nvs_get_i8 (s_nvs, j->key, (int8_t   *)j->out);
    case SG_U32: return nvs_get_u32(s_nvs, j->key, (uint32_t *)j->out);
    case SG_I32: return nvs_get_i32(s_nvs, j->key, (int32_t  *)j->out);
    case SG_U64: return nvs_get_u64(s_nvs, j->key, (uint64_t *)j->out);
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t sget(sg_type_t t, const char *key, void *out)
{
    sget_job_t j = { t, key, out };
    return ls_nvs_run(sget_job, &j, 0);
}

#define nvs_get_u8(h, k, p)  sget(SG_U8,  (k), (p))
#define nvs_get_i8(h, k, p)  sget(SG_I8,  (k), (p))
#define nvs_get_u32(h, k, p) sget(SG_U32, (k), (p))
#define nvs_get_i32(h, k, p) sget(SG_I32, (k), (p))
#define nvs_get_u64(h, k, p) sget(SG_U64, (k), (p))

static esp_err_t write_now_job(void *ctx)
{
    esp_err_t err = nvs_apply((const set_write_t *)ctx);
    return err == ESP_OK ? nvs_commit(s_nvs) : err;
}

static bool set_put(const char *key, sv_type_t type, uint64_t raw)
{
    if (!s_nvs_ok || !key || !*key) return false;

    set_write_t w;
    memset(&w, 0, sizeof(w));
    strlcpy(w.key, key, sizeof(w.key));
    w.type = type;
    switch (type) {
    case SV_U8:  w.v.u8  = (uint8_t)raw;  break;
    case SV_I8:  w.v.i8  = (int8_t)raw;   break;
    case SV_U32: w.v.u32 = raw;           break;
    case SV_I32: w.v.i32 = (int32_t)raw;  break;
    case SV_U64: w.v.u64 = raw;            break;
    }

    if (!s_wq) {
        esp_err_t err = ls_nvs_run(write_now_job, &w, 0);
        if (err != ESP_OK) ESP_LOGW(TAG, "settings write failed: %d", err);
        return err == ESP_OK;
    }

    if (xQueueSend(s_wq, &w, 0) != pdTRUE) {
        s_writes_dropped++;
        ESP_LOGW(TAG, "settings queue full; '%s' not saved", key);
        return false;
    }
    return true;
}

static inline bool sput_u8 (const char *k, uint8_t v)  { return set_put(k, SV_U8,  v); }
static inline bool sput_u32(const char *k, uint32_t v) { return set_put(k, SV_U32, v); }
static inline bool sput_i32(const char *k, int32_t v)  { return set_put(k, SV_I32, (uint32_t)v); }
static inline bool sput_u64(const char *k, uint64_t v) { return set_put(k, SV_U64, v); }

void settings_write_stats(uint32_t *done, uint32_t *dropped, uint32_t *commits)
{
    if (done)     *done     = s_writes_done;
    if (dropped)  *dropped  = s_writes_dropped;
    if (commits)  *commits  = s_commits;
}

/**/
/* Erase every key under NS.  Shares its shape with settings_reset_app() but
   applies to the whole namespace, and unlike an nvs_erase_all() the caller
   controls the commit so the version stamp goes down in the same commit as
   the wipe. */
static int erase_all_in_ns(void)
{
    char doomed[64][NVS_KEY_NAME_MAX_SIZE];
    int  n = 0;

    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && it && n < (int)(sizeof(doomed) / sizeof(doomed[0]))) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        memcpy(doomed[n], info.key, sizeof(info.key));
        doomed[n][NVS_KEY_NAME_MAX_SIZE - 1] = 0;
        n++;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    for (int i = 0; i < n; i++) nvs_erase_key(s_nvs, doomed[i]);
    return n;
}

/**/
/* Read the schema version key, classify what it says, ask the pure decider
   what to do, and act - either keep the keys, wipe them, or just stamp the
   current version onto a legacy/fresh flash.  The version stamp always goes
   down so the next boot takes the KEEP path.  Anything other than KEEP logs
   loudly: a silent reset is nearly as bad as silent corruption. */
static void settings_apply_schema(void)
{
    static const char *VER_KEY = "schema_ver";
    uint32_t stored = 0;
    esp_err_t err = nvs_get_u32(s_nvs, VER_KEY, &stored);

    settings_schema_read_status_t status;
    if (err == ESP_OK)                            status = SETTINGS_SCHEMA_READ_OK;
    else if (err == ESP_ERR_NVS_NOT_FOUND)        status = SETTINGS_SCHEMA_READ_MISSING;
    else                                          status = SETTINGS_SCHEMA_READ_UNREADABLE;

    settings_schema_action_t act = settings_schema_decide(
        status, stored, SETTINGS_SCHEMA_VERSION, SETTINGS_SCHEMA_MIN_ADDITIVE);

    switch (act) {
    case SETTINGS_SCHEMA_ACTION_KEEP:
        break;
    case SETTINGS_SCHEMA_ACTION_ADOPT:
        ESP_LOGW(TAG, "no schema version on flash; adopting v%u for existing settings",
                 (unsigned)SETTINGS_SCHEMA_VERSION);
        break;
    case SETTINGS_SCHEMA_ACTION_MIGRATE:
        ESP_LOGW(TAG, "settings schema v%u -> v%u (additive; keys kept)",
                 (unsigned)stored, (unsigned)SETTINGS_SCHEMA_VERSION);
        break;
    case SETTINGS_SCHEMA_ACTION_RESET: {
        int n = erase_all_in_ns();
        ESP_LOGW(TAG, "settings schema v%u %s at v%u; reset '%s' (%d keys erased)",
                 (unsigned)stored,
                 status == SETTINGS_SCHEMA_READ_UNREADABLE ? "unreadable" : "unsupported",
                 (unsigned)SETTINGS_SCHEMA_VERSION, NS, n);
        break;
    }
    }

    if (act != SETTINGS_SCHEMA_ACTION_KEEP) {
        nvs_set_u32(s_nvs, VER_KEY, SETTINGS_SCHEMA_VERSION);
        nvs_commit(s_nvs);
    }
}

static void settings_drop_sam_voice_keys(void);

bool settings_init(void)
{
    s_location = 0;
    s_last_fix = 0;
    __atomic_store_n(&s_auto_rotate,true,__ATOMIC_RELEASE);
    __atomic_store_n(&s_ble_at_boot,true,__ATOMIC_RELEASE);
    __atomic_store_n(&s_wifi_at_boot,true,__ATOMIC_RELEASE);
    __atomic_store_n(&s_update_check,true,__ATOMIC_RELEASE);
    home_widget_pref_init(false, 0);
    s_home_write_ready = false;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init failed: %d", err);
        return false;
    }
    if (nvs_open(NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed");
        return false;
    }
    s_nvs_ok = true;
    uint8_t key_light=1;
    nvs_get_u8(s_nvs,"key_light",&key_light);
    s_keyboard_light=key_light!=0;

    /**/  settings_apply_schema();
    settings_drop_sam_voice_keys();
    uint8_t lr_tcxo = 1, lr_dcdc = 0;
    if (nvs_get_u8(s_nvs, "lr_tcxo", &lr_tcxo) != ESP_OK || lr_tcxo > 1) lr_tcxo = 1;
    if (nvs_get_u8(s_nvs, "lr_dcdc", &lr_dcdc) != ESP_OK || lr_dcdc > 1) lr_dcdc = 0;
    __atomic_store_n(&s_lr_tcxo, lr_tcxo != 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_lr_dcdc, lr_dcdc != 0, __ATOMIC_RELEASE);

    uint8_t auto_rotate = 1;
    if(nvs_get_u8(s_nvs,"autorot",&auto_rotate)==ESP_OK)
        __atomic_store_n(&s_auto_rotate,auto_rotate!=0,__ATOMIC_RELEASE);

    uint8_t ble_boot = 1, wifi_boot = 1;
    if(nvs_get_u8(s_nvs,"bleboot",&ble_boot)==ESP_OK)
        __atomic_store_n(&s_ble_at_boot,ble_boot!=0,__ATOMIC_RELEASE);
    if(nvs_get_u8(s_nvs,"wifiboot",&wifi_boot)==ESP_OK)
        __atomic_store_n(&s_wifi_at_boot,wifi_boot!=0,__ATOMIC_RELEASE);
    uint8_t upd_check = 1;
    if(nvs_get_u8(s_nvs,"upd_check",&upd_check)==ESP_OK)
        __atomic_store_n(&s_update_check,upd_check!=0,__ATOMIC_RELEASE);

    /* settings_init runs on the cache-safe boot task in both LCD and
     * headless builds.  Load this one byte here, before p25_rx_task starts on
     * its explicit PSRAM stack; all later demod preference reads are RAM-only.
     * Schema handling stays first so a reset cannot seed the cache from a key
     * that settings_apply_schema() just invalidated. */
    uint8_t p25_demod = 0;
    esp_err_t p25_demod_err = nvs_get_u8(s_nvs, "p25_demod", &p25_demod);
    p25_demod_pref_cache_init(p25_demod_err == ESP_OK, p25_demod);

    /* HOME runs on LVGL's potentially external stack. Load its one
     * byte on the cache-safe boot task; subsequent UI reads stay in RAM. */
    uint8_t home_widget = 0;
    esp_err_t home_widget_err = nvs_get_u8(s_nvs, "home_widget", &home_widget);
    home_widget_pref_init(home_widget_err == ESP_OK, home_widget);
    uint64_t location = 0;
    esp_err_t location_err = nvs_get_u64(s_nvs, "location_v1", &location);
    int32_t lat = 0, lon = 0;
    bool legacy = nvs_get_i32(s_nvs, "home_lat", &lat) == ESP_OK &&
                  nvs_get_i32(s_nvs, "home_lon", &lon) == ESP_OK;
    s_location = location_load(location_err != ESP_ERR_NVS_NOT_FOUND,
                               location, legacy, lat, lon);
    uint64_t last_fix = 0;
    esp_err_t last_fix_err = nvs_get_u64(s_nvs, "lastfix_v1", &last_fix);
    s_last_fix = location_load(last_fix_err == ESP_OK, last_fix, false, 0, 0);
    uint64_t radio_choice = LS_RSEL_PACKED_NONE;
    if (nvs_get_u64(s_nvs, "rsel_v1", &radio_choice) == ESP_OK) {
        portENTER_CRITICAL(&s_radio_choice_lock);
        s_radio_choice = radio_choice;
        portEXIT_CRITICAL(&s_radio_choice_lock);
    }

    s_wq = xQueueCreateStatic(SET_Q_DEPTH, sizeof(set_write_t),
                              s_wq_store, &s_wq_ctrl);
    if (s_wq) {
        s_home_write_ready = xTaskCreateStatic(set_worker, "settings_wr",
            SET_STACK_WORDS, NULL, 2, s_worker_stack, &s_worker_tcb) != NULL;
    } else {
        ESP_LOGW(TAG, "write queue alloc failed - writes stay synchronous");
    }
    return true;
}

static void mk_key(char *out, size_t sz, const char *app, const char *field)
{
    char clean[9];
    int ci = 0;
    for (int i = 0; app[i] && ci < 8; i++) {
        char c = (char)tolower((unsigned char)app[i]);
        if (c != '-' && c != ' ' && c != '_') clean[ci++] = c;
    }
    clean[ci] = 0;
    snprintf(out, sz, "%s_%s", clean, field);
}

uint32_t settings_get_freq(const app_t *a)
{
    if (!s_nvs_ok || !a) return a ? a->default_freq : 0;
    char k[16]; mk_key(k, sizeof(k), a->name, "freq");
    uint32_t v = 0;
    if (nvs_get_u32(s_nvs, k, &v) == ESP_OK && v >= 1000000 && v <= 2000000000) return v;
    return a->default_freq;
}
void settings_set_freq(const app_t *a, uint32_t hz)
{
    if (!s_nvs_ok || !a) return;
    char k[16]; mk_key(k, sizeof(k), a->name, "freq");
    sput_u32(k, hz);
}

uint32_t settings_get_freq_mode(const app_t *a, int mode, uint32_t deflt)
{
    if (!s_nvs_ok || !a) return deflt;
    char field[8]; snprintf(field, sizeof(field), "freq%d", mode & 0xF);
    char k[16];     mk_key(k, sizeof(k), a->name, field);
    uint32_t v = 0;
    if (nvs_get_u32(s_nvs, k, &v) == ESP_OK && v >= 1000000 && v <= 2000000000) return v;
    return deflt;
}
void settings_set_freq_mode(const app_t *a, int mode, uint32_t hz)
{
    if (!s_nvs_ok || !a) return;
    char field[8]; snprintf(field, sizeof(field), "freq%d", mode & 0xF);
    char k[16];     mk_key(k, sizeof(k), a->name, field);
    sput_u32(k, hz);
}

int settings_get_p25_demod(void)
{
    return p25_demod_pref_cache_get();
}
void settings_set_p25_demod(int mode_idx)
{
    if (!s_nvs_ok) return;
    uint8_t stored = 0;
    if (!p25_demod_pref_cache_update(mode_idx, &stored)) return;
    sput_u8("p25_demod", stored);
}

bool settings_get_p25_auto_follow(void)
{
    if (!s_nvs_ok) return true;
    uint8_t v = 1;
    if (nvs_get_u8(s_nvs, "p25_follow", &v) != ESP_OK || v > 1) return true;
    return v != 0;
}

bool settings_set_p25_auto_follow(bool enabled)
{
    return sput_u8("p25_follow", enabled ? 1u : 0u);
}

bool settings_get_p25_skip_encrypted(void)
{
    if (!s_nvs_ok) return true;
    uint8_t v = 1;
    if (nvs_get_u8(s_nvs, "p25_skipenc", &v) != ESP_OK || v > 1) return true;
    return v != 0;
}

bool settings_set_p25_skip_encrypted(bool enabled)
{
    return sput_u8("p25_skipenc", enabled ? 1u : 0u);
}

uint32_t settings_get_p25_encrypted_skip_ms(void)
{
    if (!s_nvs_ok) return P25_CONTROL_ENCRYPTED_SKIP_DEFAULT_MS;
    uint32_t v = P25_CONTROL_ENCRYPTED_SKIP_DEFAULT_MS;
    if (nvs_get_u32(s_nvs, "p25_skipms", &v) != ESP_OK)
        return P25_CONTROL_ENCRYPTED_SKIP_DEFAULT_MS;
    return p25_controls_clamp_encrypted_skip_ms(v);
}

bool settings_set_p25_encrypted_skip_ms(uint32_t ms)
{
    return sput_u32("p25_skipms", p25_controls_clamp_encrypted_skip_ms(ms));
}

void settings_get_p25_cqpsk(p25_cqpsk_config_t *config)
{
    if (!config) return;
    p25_cqpsk_config_defaults(config);
    if (!s_nvs_ok) return;

    uint64_t packed = 0;
    if (nvs_get_u64(s_nvs, "p25_cqloop", &packed) != ESP_OK) return;
    p25_cqpsk_config_t candidate;
    if (!p25_cqpsk_config_unpack(packed, &candidate)) return;
    *config = candidate;
}

bool settings_set_p25_cqpsk(const p25_cqpsk_config_t *config)
{
    uint64_t packed = 0;
    return p25_cqpsk_config_pack(config, &packed) &&
           sput_u64("p25_cqloop", packed);
}

int settings_get_gain(const app_t *a)
{
    if (!s_nvs_ok || !a) return a ? a->default_gain : 0;
    char k[16]; mk_key(k, sizeof(k), a->name, "gain");
    int32_t v = 0;
    if (nvs_get_i32(s_nvs, k, &v) == ESP_OK && v >= 0 && v <= 600) return (int)v;
    return a->default_gain;
}
void settings_set_gain(const app_t *a, int tenths)
{
    if (!s_nvs_ok || !a) return;
    char k[16]; mk_key(k, sizeof(k), a->name, "gain");
    sput_i32(k, (int32_t)tenths);
}

int settings_fav_count(const app_t *a)
{
    int n = 0;
    for (int i = 0; i < MAX_FAVOURITES; i++)
        if (settings_fav_get(a, i) != 0) n++;
    return n;
}
uint32_t settings_fav_get(const app_t *a, int slot)
{
    if (!s_nvs_ok || !a || slot < 0 || slot >= MAX_FAVOURITES) return 0;
    char field[8]; snprintf(field, sizeof(field), "fav%d", slot);
    char k[16]; mk_key(k, sizeof(k), a->name, field);
    uint32_t v = 0;
    nvs_get_u32(s_nvs, k, &v);
    return v;
}
void settings_fav_set(const app_t *a, int slot, uint32_t hz)
{
    if (!s_nvs_ok || !a || slot < 0 || slot >= MAX_FAVOURITES) return;
    char field[8]; snprintf(field, sizeof(field), "fav%d", slot);
    char k[16]; mk_key(k, sizeof(k), a->name, field);
    sput_u32(k, hz);
}
void settings_fav_clear(const app_t *a, int slot) { settings_fav_set(a, slot, 0); }

bool settings_get_home(float *lat, float *lon)
{
    portENTER_CRITICAL(&s_location_lock);
    uint64_t location = s_location;
    portEXIT_CRITICAL(&s_location_lock);
    return location_unpack(location, lat, lon);
}
static bool settings_put_location(uint64_t location)
{
    if (!s_home_write_ready || !sput_u64("location_v1", location)) return false;
    portENTER_CRITICAL(&s_location_lock);
    s_location = location;
    portEXIT_CRITICAL(&s_location_lock);
    return true;
}
bool settings_set_home(float lat, float lon)
{
    uint64_t location;
    return location_pack(lat, lon, &location) && settings_put_location(location);
}
bool settings_clear_home(void) { return settings_put_location(0); }

/* Stored in tenths of a degree; this value means none (NVS keys cannot be
   removed through the deferred write queue). */
#define DF_OFFSET_NONE 0x7FFFFFFFu

bool settings_get_df_offset(int source, int method, float *degrees)
{
    if (!s_nvs_ok || source < 0 || source > 15 || method < 0 || method > 1) return false;
    char k[12]; snprintf(k, sizeof(k), "dfo%d_%d", source, method);
    uint32_t v = DF_OFFSET_NONE;
    if (nvs_get_u32(s_nvs, k, &v) != ESP_OK || v == DF_OFFSET_NONE) return false;
    const float d = (int32_t)v / 10.0f;
    if (!isfinite(d) || d < -180 || d > 180) return false;
    if (degrees) *degrees = d;
    return true;
}

void settings_set_df_offset(int source, int method, float degrees)
{
    if (!s_nvs_ok || source < 0 || source > 15 || method < 0 || method > 1) return;
    char k[12]; snprintf(k, sizeof(k), "dfo%d_%d", source, method);
    const uint32_t v = isfinite(degrees) && degrees >= -180 && degrees <= 180
        ? (uint32_t)(int32_t)lroundf(degrees * 10.0f) : DF_OFFSET_NONE;
    sput_u32(k, v);
}

/* Five u64 keys: bytes 0-35 the pattern, 36 the circles (capped at 255),
   37-39 a tag so a stray key never reads as a pattern. */
bool settings_get_df_pattern(int source, int8_t pattern[36], uint16_t *circles)
{
    if (!s_nvs_ok || !pattern || source < 0 || source > 15) return false;
    uint8_t b[40];
    for (int i = 0; i < 5; i++) {
        char k[12]; snprintf(k, sizeof(k), "dfp%d_%d", source, i);
        uint64_t v;
        if (nvs_get_u64(s_nvs, k, &v) != ESP_OK) return false;
        for (int j = 0; j < 8; j++) b[i * 8 + j] = (uint8_t)(v >> (8 * j));
    }
    if (b[37] != 'P' || b[38] != 'T' || b[39] != 1 || !b[36]) return false;
    memcpy(pattern, b, 36);
    if (circles) *circles = b[36];
    return true;
}

void settings_set_df_pattern(int source, const int8_t pattern[36], uint16_t circles)
{
    if (!s_nvs_ok || source < 0 || source > 15) return;
    uint8_t b[40] = { 0 };
    if (pattern && circles) memcpy(b, pattern, 36);
    b[36] = (uint8_t)(circles > 255 ? 255 : circles);
    b[37] = 'P'; b[38] = 'T'; b[39] = 1;
    for (int i = 0; i < 5; i++) {
        char k[12]; snprintf(k, sizeof(k), "dfp%d_%d", source, i);
        uint64_t v = 0;
        for (int j = 0; j < 8; j++) v |= (uint64_t)b[i * 8 + j] << (8 * j);
        sput_u64(k, v);
    }
}

int settings_get_df_option(int id, int fallback)
{
    if (!s_nvs_ok || id < 0 || id > 31) return fallback;
    char k[8]; snprintf(k, sizeof(k), "dfx%d", id);
    uint8_t v;
    return nvs_get_u8(s_nvs, k, &v) == ESP_OK ? v : fallback;
}

void settings_set_df_option(int id, int value)
{
    if (!s_nvs_ok || id < 0 || id > 31 || value < 0 || value > 255) return;
    char k[8]; snprintf(k, sizeof(k), "dfx%d", id);
    sput_u8(k, (uint8_t)value);
}

#define DF_CHANNELS_MAX 8
int settings_get_df_channels(int slot, uint32_t *hz, int max)
{
    if (!s_nvs_ok || !hz || slot < 0 || slot > 1) return 0;
    char k[8]; snprintf(k, sizeof(k), "dfn%d", slot);
    uint8_t n = 0;
    if (nvs_get_u8(s_nvs, k, &n) != ESP_OK) return 0;
    if (n > DF_CHANNELS_MAX) n = DF_CHANNELS_MAX;
    int got = 0;
    for (int i = 0; i < n && got < max; i++) {
        snprintf(k, sizeof(k), "dfc%d_%d", slot, i);
        uint32_t v;
        if (nvs_get_u32(s_nvs, k, &v) == ESP_OK && v) hz[got++] = v;
    }
    return got;
}

void settings_set_df_channels(int slot, const uint32_t *hz, int n)
{
    if (!s_nvs_ok || !hz || slot < 0 || slot > 1 || n < 0) return;
    if (n > DF_CHANNELS_MAX) n = DF_CHANNELS_MAX;
    char k[8];
    /* Only what changed goes on the write queue, which is short. */
    uint32_t old[DF_CHANNELS_MAX];
    const int had = settings_get_df_channels(slot, old, DF_CHANNELS_MAX);
    for (int i = 0; i < n; i++)
        if (i >= had || old[i] != hz[i]) { snprintf(k, sizeof(k), "dfc%d_%d", slot, i); sput_u32(k, hz[i]); }
    if (had != n) { snprintf(k, sizeof(k), "dfn%d", slot); sput_u8(k, (uint8_t)n); }
}

bool settings_get_last_fix(float *lat, float *lon)
{
    portENTER_CRITICAL(&s_location_lock);
    uint64_t location = s_last_fix;
    portEXIT_CRITICAL(&s_location_lock);
    return location_unpack(location, lat, lon);
}
bool settings_set_last_fix(float lat, float lon)
{
    uint64_t location;
    if (!location_pack(lat, lon, &location) || !s_home_write_ready ||
        !sput_u64("lastfix_v1", location)) return false;
    portENTER_CRITICAL(&s_location_lock);
    s_last_fix = location;
    portEXIT_CRITICAL(&s_location_lock);
    return true;
}

int settings_get_radio_choice(int job)
{
    portENTER_CRITICAL(&s_radio_choice_lock);
    const uint64_t packed = s_radio_choice;
    portEXIT_CRITICAL(&s_radio_choice_lock);
    return ls_rsel_unpack(packed, job);
}

bool settings_set_radio_choice(int job, int radio)
{
    if (job < 0 || job > 15 || radio < -1 || radio > 14) return false;
    portENTER_CRITICAL(&s_radio_choice_lock);
    const uint64_t was = s_radio_choice;
    const uint64_t now = ls_rsel_pack(was, job, radio);
    s_radio_choice = now;
    portEXIT_CRITICAL(&s_radio_choice_lock);
    /* The same choice again is not a write. */
    return now == was || sput_u64("rsel_v1", now);
}

int settings_get_brightness(void)
{
    if (!s_nvs_ok) return 80;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "brightness", &v) != ESP_OK || v < 5 || v > 100) return 80;
    return (int)v;
}
void settings_set_brightness(int pct)
{
    if (!s_nvs_ok) return;
    if (pct < 5)   pct = 5;
    if (pct > 100) pct = 100;
    sput_u8("brightness", (uint8_t)pct);
}

#if LS_HAS_COMPACT_UI
#define AUTODIM_DEFAULT false
#else
#define AUTODIM_DEFAULT true
#endif

bool settings_get_autodim(void)
{
    if (!s_nvs_ok) return AUTODIM_DEFAULT;
    uint8_t v = AUTODIM_DEFAULT ? 1 : 0;
    if (nvs_get_u8(s_nvs, "autodim", &v) != ESP_OK) return AUTODIM_DEFAULT;
    return v != 0;
}
void settings_set_autodim(bool en)
{
    if (!s_nvs_ok) return;
    sput_u8("autodim", en ? 1 : 0);
}
int settings_get_autodim_timeout(void)
{
    if (!s_nvs_ok) return 120;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "autodim_to", &v) != ESP_OK || v < 5) return 120;
    return (int)v;
}
void settings_set_autodim_timeout(int seconds)
{
    if (!s_nvs_ok) return;
    if (seconds < 5)   seconds = 5;
    if (seconds > 240) seconds = 240;
    sput_u8("autodim_to", (uint8_t)seconds);
}

int settings_get_volume(void)
{
    if (!s_nvs_ok) return 35;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "volume", &v) != ESP_OK || v > 100) return 35;
    return (int)v;
}
void settings_set_volume(int pct)
{
    if (!s_nvs_ok) return;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    sput_u8("volume", (uint8_t)pct);
}

int settings_get_boot_sound(void)
{
    if (!s_nvs_ok) return 1;
    uint8_t v = 1;
    if (nvs_get_u8(s_nvs, "boot_snd", &v) != ESP_OK || v > 2) return 1;
    return (int)v;
}
void settings_set_boot_sound(int mode)
{
    if (!s_nvs_ok) return;
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    sput_u8("boot_snd", (uint8_t)mode);
}

/**/
bool settings_get_usb_autoreboot(void)
{
    if (!s_nvs_ok) return false;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "usb_autoreb", &v) != ESP_OK) return false;
    return v != 0;
}
void settings_set_usb_autoreboot(bool en)
{
    if (!s_nvs_ok) return;
    sput_u8("usb_autoreb", en ? 1 : 0);
}

/* Internal until told otherwise. See the header for why that is the
   default and not merely the first option. */
bool settings_get_antenna_external(void)
{
    if (!s_nvs_ok) return false;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "ant_ext", &v) != ESP_OK) return false;
    return v != 0;
}
void settings_set_antenna_external(bool external)
{
    if (!s_nvs_ok) return;
    sput_u8("ant_ext", external ? 1 : 0);
}

/* The same byte before settings_init(), so the route is set before the
   radio or the mesh can transmit. Read-only, internal when unset. */
bool settings_peek_antenna_external(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    const esp_err_t err = (nvs_get_u8)(h, "ant_ext", &v);
    nvs_close(h);
    return err == ESP_OK && v != 0;
}

int settings_get_p25_lr_gain(void)
{
    if (!s_nvs_ok) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "p25_lrgain", &v) != ESP_OK || v > 13) return 0;
    return (int)v;
}
void settings_set_p25_lr_gain(int step)
{
    if (!s_nvs_ok) return;
    if (step < 0)  step = 0;
    if (step > 13) step = 13;
    sput_u8("p25_lrgain", (uint8_t)step);
}

bool settings_get_lr_tcxo(void)
{
    return __atomic_load_n(&s_lr_tcxo, __ATOMIC_ACQUIRE);
}
bool settings_set_lr_tcxo(bool tcxo)
{
    if (!sput_u8("lr_tcxo", tcxo ? 1u : 0u)) return false;
    __atomic_store_n(&s_lr_tcxo, tcxo, __ATOMIC_RELEASE);
    return true;
}
bool settings_get_lr_dcdc(void)
{
    return __atomic_load_n(&s_lr_dcdc, __ATOMIC_ACQUIRE);
}
bool settings_set_lr_dcdc(bool dcdc)
{
    if (!sput_u8("lr_dcdc", dcdc ? 1u : 0u)) return false;
    __atomic_store_n(&s_lr_dcdc, dcdc, __ATOMIC_RELEASE);
    return true;
}

/* The threshold default. Twelve dB was too close to the noise: thermal
   spread across a sweep is several dB, so nearly every bin eventually
   crossed it, the detection list filled with grass, and it buzzed once per
   bin on the way. Twenty-five leaves the noise well clear and is still far
   below anything worth catching - the reference signal this was built
   against sits about sixty dB over the floor. */
#define SUBGHZ_GATE_DEFAULT 25

int settings_get_subghz_gate_db(void)
{
    if (!s_nvs_ok) return SUBGHZ_GATE_DEFAULT;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "sg_gate", &v) != ESP_OK || !v)
        return SUBGHZ_GATE_DEFAULT;
    return v;
}
void settings_set_subghz_gate_db(int db)
{
    if (!s_nvs_ok || db < 1 || db > 60) return;
    sput_u8("sg_gate", (uint8_t)db);
}
int settings_get_subghz_on_hit(void)
{
    if (!s_nvs_ok) return 1;                       /* buzz */
    uint8_t v = 1;
    if (nvs_get_u8(s_nvs, "sg_onhit", &v) != ESP_OK) return 1;
    return v > 2 ? 1 : v;
}
void settings_set_subghz_on_hit(int mode)
{
    if (!s_nvs_ok || mode < 0 || mode > 2) return;
    sput_u8("sg_onhit", (uint8_t)mode);
}
/* COMPASS: bit 0 simple style, bit 1 magnetic rather than true. */
int settings_get_compass_options(void)
{
    if (!s_nvs_ok) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "compass_opt", &v) != ESP_OK) return 0;
    return v & 3;
}
void settings_set_compass_options(int options)
{
    if (!s_nvs_ok || options < 0 || options > 3) return;
    sput_u8("compass_opt", (uint8_t)options);
}
/* MAP: which overlay layers are shown, as bits; `fallback` when unset. */
uint32_t settings_get_waterfall(uint32_t fallback)
{
    if (!s_nvs_ok) return fallback;
    uint32_t v;
    return nvs_get_u32(s_nvs, "wf_display", &v) == ESP_OK ? v : fallback;
}
void settings_set_waterfall(uint32_t value)
{
    (void)sput_u32("wf_display", value);
}
uint32_t settings_get_map_layers(uint32_t fallback)
{
    if (!s_nvs_ok) return fallback;
    uint32_t v = 0;
    if (nvs_get_u32(s_nvs, "map_layers", &v) != ESP_OK) return fallback;
    return v;
}
void settings_set_map_layers(uint32_t layers)
{
    if (!s_nvs_ok) return;
    sput_u32("map_layers", layers);
}
/* ADS-B: the mini map's FOLLOW mode; -1 when unset. */
int settings_get_adsb_follow(void)
{
    if (!s_nvs_ok) return -1;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "adsb_follow", &v) != ESP_OK || v > 15) return -1;
    return (int)v;
}
void settings_set_adsb_follow(int mode)
{
    if (!s_nvs_ok || mode < 0 || mode > 15) return;
    sput_u8("adsb_follow", (uint8_t)mode);
}
int settings_get_subghz_style(void)
{
    if (!s_nvs_ok) return 2;                       /* bars */
    uint8_t v = 2;
    if (nvs_get_u8(s_nvs, "sg_style", &v) != ESP_OK) return 2;
    return v;
}
void settings_set_subghz_style(int style)
{
    if (!s_nvs_ok || style < 0 || style > 8) return;
    sput_u8("sg_style", (uint8_t)style);
}
int settings_get_subghz_colour(void)
{
    if (!s_nvs_ok) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "sg_col", &v) != ESP_OK) return 0;
    return v;
}
void settings_set_subghz_colour(int colour)
{
    if (!s_nvs_ok || colour < 0 || colour > 8) return;
    sput_u8("sg_col", (uint8_t)colour);
}
uint32_t settings_get_rec(const char *name, uint32_t deflt)
{
    char key[16];
    if (!s_nvs_ok || !name || snprintf(key, sizeof(key), "rec_%s", name) >= (int)sizeof(key))
        return deflt;
    uint32_t v = deflt;
    return nvs_get_u32(s_nvs, key, &v) == ESP_OK ? v : deflt;
}

void settings_set_rec(const char *name, uint32_t value)
{
    char key[16];
    if (!name || snprintf(key, sizeof(key), "rec_%s", name) >= (int)sizeof(key)) return;
    (void)sput_u32(key, value);
}

void settings_get_subghz_fsk(uint32_t *bitrate, uint32_t *deviation_hz,
                             uint32_t *sync_word, int *preamble_bits,
                             int *bandwidth_khz)
{
    uint32_t br = 4800, dev = 25000, sync = 0x2DD42DD4u;
    uint32_t pre32 = 32, bw = 59;
    if (s_nvs_ok) {
        (void)nvs_get_u32(s_nvs, "sg_br",   &br);
        (void)nvs_get_u32(s_nvs, "sg_dev",  &dev);
        (void)nvs_get_u32(s_nvs, "sg_sync", &sync);
        /* sg_pre held a uint8_t and the screen offers up to 1024, so
           anything past 255 could never be written back. sg_pre32 is
           the real range; the old key is still read so a preamble set
           before this survives the upgrade. */
        if (nvs_get_u32(s_nvs, "sg_pre32", &pre32) != ESP_OK) {
            uint8_t legacy = 0;
            if (nvs_get_u8(s_nvs, "sg_pre", &legacy) == ESP_OK && legacy)
                pre32 = legacy;
        }
        (void)nvs_get_u32(s_nvs, "sg_bw",   &bw);
    }
    if (bitrate)       *bitrate = br;
    if (deviation_hz)  *deviation_hz = dev;
    if (sync_word)     *sync_word = sync;
    if (preamble_bits) *preamble_bits = pre32 ? (int)pre32 : 32;
    if (bandwidth_khz) *bandwidth_khz = bw ? (int)bw : 59;
}
void settings_set_subghz_fsk(uint32_t bitrate, uint32_t deviation_hz,
                             uint32_t sync_word, int preamble_bits,
                             int bandwidth_khz)
{
    if (!s_nvs_ok) return;
    sput_u32("sg_br", bitrate);
    sput_u32("sg_dev", deviation_hz);
    sput_u32("sg_sync", sync_word);
    /* 1024 is what the screen offers and what rec_watch_fsk_set
       accepts, so that is what has to fit here. */
    if (preamble_bits > 0 && preamble_bits <= 1024)
        sput_u32("sg_pre32", (uint32_t)preamble_bits);
    if (bandwidth_khz > 0 && bandwidth_khz <= 1000)
        sput_u32("sg_bw", (uint32_t)bandwidth_khz);
}

bool settings_get_alert_ring(void)
{
    if (!s_nvs_ok) return true;
    uint8_t v = 1;
    if (nvs_get_u8(s_nvs, "alert_ring", &v) != ESP_OK) return true;
    return v != 0;
}
void settings_set_alert_ring(bool en)
{
    if (!s_nvs_ok) return;
    sput_u8("alert_ring", en ? 1 : 0);
}
bool settings_get_alert_vibe(void)
{
    if (!s_nvs_ok) return true;
    uint8_t v = 1;
    if (nvs_get_u8(s_nvs, "alert_vibe", &v) != ESP_OK) return true;
    return v != 0;
}
void settings_set_alert_vibe(bool en)
{
    if (!s_nvs_ok) return;
    sput_u8("alert_vibe", en ? 1 : 0);
}

/**/
typedef struct { const char *pfx; size_t plen; int n; } reset_job_t;

static esp_err_t reset_app_job(void *ctx)
{
    reset_job_t *r = (reset_job_t *)ctx;
    char doomed[24][NVS_KEY_NAME_MAX_SIZE];
    int  n = 0;

    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && it && n < (int)(sizeof(doomed) / sizeof(doomed[0]))) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, r->pfx, r->plen) == 0) {
            memcpy(doomed[n], info.key, sizeof(info.key));
            doomed[n][NVS_KEY_NAME_MAX_SIZE - 1] = 0;
            n++;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    for (int i = 0; i < n; i++) nvs_erase_key(s_nvs, doomed[i]);
    if (n) nvs_commit(s_nvs);
    r->n = n;
    return ESP_OK;
}

void settings_reset_app(const app_t *a)
{
    if (!s_nvs_ok || !a || !a->name) return;

    char pfx[16];
    mk_key(pfx, sizeof(pfx), a->name, "");
    const size_t plen = strlen(pfx);
    if (plen < 2) return;

    /* Callable from a PSRAM- or TCM-stack UI/console task. */
    reset_job_t job = { pfx, plen, 0 };
    if (ls_nvs_run(reset_app_job, &job, 0) != ESP_OK) {
        ESP_LOGW(TAG, "reset '%s' not done - no cache-safe stack", a->name);
        return;
    }
    const int n = job.n;

    ESP_LOGW(TAG, "reset '%s' to defaults (%d keys erased)", a->name, n);
}

/**/
/* The C6 and its BLE stack come up before settings_init() runs, so the
   ordinary getter would still be reading its default when the decision is
   made. This opens the namespace read-only for the one byte. Defaults to on,
   which is what every unit did before the preference existed. */
static bool peek_on_by_default(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return true;
    uint8_t v = 1;
    const esp_err_t err = (nvs_get_u8)(h, key, &v);
    nvs_close(h);
    return err == ESP_OK ? v != 0 : true;
}

bool settings_peek_ble_at_boot(void)
{
    return peek_on_by_default("bleboot");
}

/* Wi-Fi's autojoin is decided at the same point as BLE, and it was read
   through the ordinary getter: 'radios wifi off' stored the byte, 'radios'
   read it back as off, and every boot still rejoined, because the getter
   was answering from its default half a second before settings_init(). */
bool settings_peek_wifi_at_boot(void)
{
    return peek_on_by_default("wifiboot");
}

bool settings_get_ble_at_boot(void)
{
    return __atomic_load_n(&s_ble_at_boot,__ATOMIC_ACQUIRE);
}
void settings_set_ble_at_boot(bool enabled)
{
    if (s_nvs_ok && sput_u8("bleboot", enabled ? 1 : 0))
        __atomic_store_n(&s_ble_at_boot,enabled,__ATOMIC_RELEASE);
}
bool settings_get_wifi_at_boot(void)
{
    return __atomic_load_n(&s_wifi_at_boot,__ATOMIC_ACQUIRE);
}
void settings_set_wifi_at_boot(bool enabled)
{
    if (s_nvs_ok && sput_u8("wifiboot", enabled ? 1 : 0))
        __atomic_store_n(&s_wifi_at_boot,enabled,__ATOMIC_RELEASE);
}

bool settings_get_update_check(void)
{
    return __atomic_load_n(&s_update_check,__ATOMIC_ACQUIRE);
}
void settings_set_update_check(bool daily)
{
    if (s_nvs_ok && sput_u8("upd_check", daily ? 1 : 0))
        __atomic_store_n(&s_update_check,daily,__ATOMIC_RELEASE);
}

bool settings_get_auto_rotate(void)
{
    return __atomic_load_n(&s_auto_rotate,__ATOMIC_ACQUIRE);
}
void settings_set_auto_rotate(bool enabled)
{
    if (s_nvs_ok && sput_u8("autorot", enabled ? 1 : 0))
        __atomic_store_n(&s_auto_rotate,enabled,__ATOMIC_RELEASE);
}

/**/
bool settings_get_nav_autohide(void)
{
    uint8_t value=1;
    if(s_nvs_ok)nvs_get_u8(s_nvs,"nav_auto",&value);
    return value!=0;
}
void settings_set_nav_autohide(bool enabled)
{
    if(s_nvs_ok)sput_u8("nav_auto",enabled?1:0);
}
int settings_get_theme(void)
{
    if (!s_nvs_ok) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "ui_theme", &v) != ESP_OK) return 0;
    return (int)v;
}

int settings_get_home_widget(void) { return home_widget_pref_get(); }

static bool save_home_widget(int widget)
{
    /* Never fall back to synchronous flash access on the UI stack. */
    return s_home_write_ready && s_wq && sput_u8("home_widget", (uint8_t)widget);
}

bool settings_set_home_widget(int widget)
{
    return home_widget_pref_set(widget, save_home_widget);
}

/**/
void settings_set_theme(int theme)
{
    if (!s_nvs_ok) return;
    if (theme < 0) theme = 0;
    sput_u8("ui_theme", (uint8_t)theme);
}

/* See the header. An absent key reads as off, so a board that has
   never been told comes up on the black it always has. */
bool settings_get_daylight(void)
{
    if (!s_nvs_ok) return false;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "ui_daylight", &v) != ESP_OK) return false;
    return v != 0;
}
bool settings_get_keyboard_dim(void)
{
    if (!s_nvs_ok) return true;
    uint8_t v = 1;
    if (nvs_get_u8(s_nvs, "key_dim", &v) != ESP_OK) return true;
    return v != 0;
}
void settings_set_keyboard_dim(bool on)
{
    if (s_nvs_ok) sput_u8("key_dim", on ? 1 : 0);
}
bool settings_get_keyboard_light(void) { return s_keyboard_light; }
void settings_set_keyboard_light(bool on)
{
    s_keyboard_light=on;
    if(s_nvs_ok) sput_u8("key_light",on?1:0);
}
void settings_set_daylight(bool on)
{
    if (!s_nvs_ok) return;
    sput_u8("ui_daylight", on ? 1 : 0);
}

/**/
int settings_get_scan_zone(void)
{
    if (!s_nvs_ok) return 0;
    int8_t v = 0;
    if (nvs_get_i8(s_nvs, "scan_zone", &v) != ESP_OK) return 0;
    return (int)v;
}

uint8_t settings_get_scan_options(void)
{
    uint8_t value = 0;
    if (!s_nvs_ok || nvs_get_u8(s_nvs, "scan_options", &value) != ESP_OK) return 0;
    return value & 3;
}
bool settings_set_scan_options(uint8_t options) { return sput_u8("scan_options", options & 3); }

/**/
void settings_set_scan_zone(int zone)
{
    if (!s_nvs_ok) return;
    if (zone < -1) zone = -1;
    (void)set_put("scan_zone", SV_I8, (uint64_t)(int64_t)zone);
}

/* Speech voice by stable id (speech_voice_t); out-of-range reads as 0. */
int settings_speech_voice_get(void)
{
    if (!s_nvs_ok) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "speech_voice", &v) != ESP_OK) return 0;
    return (int)v;
}
void settings_speech_voice_set(int v)
{
    if (!s_nvs_ok || v < 0 || v > 255) return;
    sput_u8("speech_voice", (uint8_t)v);
}

/* Callout modes, packed by audio_events.c. False when never saved. */
bool settings_get_callouts(uint32_t *out)
{
    if (!s_nvs_ok || !out) return false;
    return nvs_get_u32(s_nvs, "callouts", out) == ESP_OK;
}
void settings_set_callouts(uint32_t packed)
{
    if (s_nvs_ok) sput_u32("callouts", packed);
}

int settings_hackrf_ppm_get(void)
{
    if (!s_nvs_ok) return 0;
    int32_t v = 0;
    if (nvs_get_i32(s_nvs, "hackrf_ppm", &v) != ESP_OK || v < -200 || v > 200) return 0;
    return (int)v;
}
void settings_hackrf_ppm_set(int ppm)
{
    if (s_nvs_ok && ppm >= -200 && ppm <= 200) sput_i32("hackrf_ppm", ppm);
}

int settings_speech_volume_get(void)
{
    if (!s_nvs_ok) return 100;
    uint8_t v = 100;
    if (nvs_get_u8(s_nvs, "speech_vol", &v) != ESP_OK || v > 100) return 100;
    return (int)v;
}
void settings_speech_volume_set(int pct)
{
    if (!s_nvs_ok || pct < 0 || pct > 100) return;
    sput_u8("speech_vol", (uint8_t)pct);
}

/* voice_preset, voice_lp and voice_shelf configured the SAM engine. Their
   numbers mean nothing to the formant voice, so they are erased rather than
   read as something else. */
static void settings_drop_sam_voice_keys(void)
{
    static const char *const keys[] = { "voice_preset", "voice_lp", "voice_shelf" };
    bool erased = false;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (nvs_erase_key(s_nvs, keys[i]) == ESP_OK) erased = true;
    if (erased) {
        nvs_commit(s_nvs);
        ESP_LOGI(TAG, "SAM voice settings removed");
    }
}

static int eq_get_i(const char *key, int deflt, int lo, int hi)
{
    if (!s_nvs_ok) return deflt;
    int8_t v = 0;
    if (nvs_get_i8(s_nvs, key, &v) != ESP_OK) return deflt;
    if (v < lo || v > hi) return deflt;
    return (int)v;
}

static void eq_set_i(const char *key, int v, int lo, int hi)
{
    if (!s_nvs_ok) return;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    set_put(key, SV_I8, (uint32_t)(int32_t)v);
}

int  settings_eq_preset_get(void)      { return eq_get_i("eq_preset", 1,   0,  4);   }
void settings_eq_preset_set(int v)     { eq_set_i("eq_preset", v,   0,  4);          }
int  settings_eq_hp_get(void)          { return eq_get_i("eq_hp",    18,  0,  40);   }
void settings_eq_hp_set(int v)         { eq_set_i("eq_hp",    v,   0,  40);          }
int  settings_eq_bass_get(void)        { return eq_get_i("eq_bass",  6,  -6,  12);   }
void settings_eq_bass_set(int v)       { eq_set_i("eq_bass",  v,  -6,  12);          }
int  settings_eq_treb_get(void)        { return eq_get_i("eq_treb", -2,  -8,   8);   }
void settings_eq_treb_set(int v)       { eq_set_i("eq_treb",  v,  -8,   8);          }
int  settings_eq_punch_get(void)       { return eq_get_i("eq_punch", 30,  0, 100);   }
void settings_eq_punch_set(int v)      { eq_set_i("eq_punch", v,   0, 100);          }
int  settings_eq_loud_get(void)        { return eq_get_i("eq_loud",  1,   0,   3);   }
void settings_eq_loud_set(int v)       { eq_set_i("eq_loud",  v,   0,   3);          }
