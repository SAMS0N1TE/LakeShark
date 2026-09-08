#include "settings.h"
#include "settings_schema.h"
#include "home_widget_pref.h"
#include "location_pref.h"
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

static const char  *TAG      = "settings";
static const char  *NS       = "sdr-tool";
static nvs_handle_t s_nvs    = 0;
static bool         s_nvs_ok = false;
static bool         s_home_write_ready = false;
static uint64_t s_location;
static portMUX_TYPE s_location_lock = portMUX_INITIALIZER_UNLOCKED;

#define SET_Q_DEPTH     16
#define SET_PENDING_MAX 16
#define SET_QUIET_MS    300
/*LS-806  4096 with 3172 unused - about 924 B in use. */
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
        esp_err_t err = nvs_apply(&w);
        if (err == ESP_OK) err = nvs_commit(s_nvs);
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

/*LS-800*/
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

/*LS-800*/
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

bool settings_init(void)
{
    s_location = 0;
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

    /*LS-800*/  settings_apply_schema();

    /* LS-719: settings_init runs on the cache-safe boot task in both LCD and
     * headless builds.  Load this one byte here, before p25_rx_task starts on
     * its explicit PSRAM stack; all later demod preference reads are RAM-only.
     * Schema handling stays first so a reset cannot seed the cache from a key
     * that settings_apply_schema() just invalidated. */
    uint8_t p25_demod = 0;
    esp_err_t p25_demod_err = nvs_get_u8(s_nvs, "p25_demod", &p25_demod);
    p25_demod_pref_cache_init(p25_demod_err == ESP_OK, p25_demod);

    /* LS-763: HOME runs on LVGL's potentially external stack. Load its one
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

/*
 * Task 655: P25 demodulator mode selection. Global, not per-app, because
 * there is only one P25 app and the mode is a property of the receive
 * chain rather than of any particular frequency preset. Returns -1 when
 * the key is absent so the caller can distinguish "user has never picked"
 * from "user picked mode 0".
 */
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

bool settings_get_autodim(void)
{
    if (!s_nvs_ok) return true;
    uint8_t v = 1;
    if (nvs_get_u8(s_nvs, "autodim", &v) != ESP_OK) return true;
    return v != 0;
}
void settings_set_autodim(bool en)
{
    if (!s_nvs_ok) return;
    sput_u8("autodim", en ? 1 : 0);
}
int settings_get_autodim_timeout(void)
{
    if (!s_nvs_ok) return 30;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "autodim_to", &v) != ESP_OK || v < 5) return 30;
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

/*LS-770*/
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

/*LS-608*/
void settings_reset_app(const app_t *a)
{
    if (!s_nvs_ok || !a || !a->name) return;

    char pfx[16];
    mk_key(pfx, sizeof(pfx), a->name, "");
    const size_t plen = strlen(pfx);
    if (plen < 2) return;

    char doomed[24][NVS_KEY_NAME_MAX_SIZE];
    int  n = 0;

    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && it && n < (int)(sizeof(doomed) / sizeof(doomed[0]))) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, pfx, plen) == 0) {
            memcpy(doomed[n], info.key, sizeof(info.key));
            doomed[n][NVS_KEY_NAME_MAX_SIZE - 1] = 0;
            n++;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    for (int i = 0; i < n; i++) nvs_erase_key(s_nvs, doomed[i]);
    if (n) nvs_commit(s_nvs);

    ESP_LOGW(TAG, "reset '%s' to defaults (%d keys erased)", a->name, n);
}

/*LS-606*/
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

/*LS-606*/
void settings_set_theme(int theme)
{
    if (!s_nvs_ok) return;
    if (theme < 0) theme = 0;
    sput_u8("ui_theme", (uint8_t)theme);
}

/*LS-703*/
int settings_get_scan_zone(void)
{
    if (!s_nvs_ok) return 0;
    int8_t v = 0;
    if (nvs_get_i8(s_nvs, "scan_zone", &v) != ESP_OK) return 0;
    return (int)v;
}

/*LS-703*/
void settings_set_scan_zone(int zone)
{
    if (!s_nvs_ok) return;
    if (zone < -1) zone = -1;
    if (nvs_set_i8(s_nvs, "scan_zone", (int8_t)zone) == ESP_OK) nvs_commit(s_nvs);
}

int settings_voice_preset_get(void)
{
    if (!s_nvs_ok) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "voice_preset", &v) != ESP_OK) return 0;
    return (int)v;
}
void settings_voice_preset_set(int p)
{
    if (!s_nvs_ok || p < 0 || p > 255) return;
    sput_u8("voice_preset", (uint8_t)p);
}
int settings_voice_lowpass_get(void)
{
    if (!s_nvs_ok) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "voice_lp", &v) != ESP_OK) return 0;
    return (int)v;
}
void settings_voice_lowpass_set(int m)
{
    if (!s_nvs_ok || m < 0 || m > 2) return;
    sput_u8("voice_lp", (uint8_t)m);
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

int settings_voice_lowshelf_get(void)
{
    if (!s_nvs_ok) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "voice_shelf", &v) != ESP_OK) return 0;
    return (int)v;
}
void settings_voice_lowshelf_set(int m)
{
    if (!s_nvs_ok || m < 0 || m > 2) return;
    sput_u8("voice_shelf", (uint8_t)m);
}
