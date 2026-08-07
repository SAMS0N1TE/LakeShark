#include "settings.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char  *TAG      = "settings";
static const char  *NS       = "sdr-tool";
static nvs_handle_t s_nvs    = 0;
static bool         s_nvs_ok = false;

#define SET_Q_DEPTH     16
#define SET_PENDING_MAX 16
#define SET_QUIET_MS    300
#define SET_STACK_WORDS (4096 / sizeof(StackType_t))

typedef enum { SV_U8, SV_U32, SV_I32 } sv_type_t;

typedef struct {
    char      key[NVS_KEY_NAME_MAX_SIZE];
    sv_type_t type;
    union { uint8_t u8; uint32_t u32; int32_t i32; } v;
} set_write_t;

static QueueHandle_t   s_wq = NULL;
static StaticQueue_t   s_wq_ctrl;
static uint8_t         s_wq_store[SET_Q_DEPTH * sizeof(set_write_t)];
static StackType_t     s_worker_stack[SET_STACK_WORDS];
static StaticTask_t    s_worker_tcb;
static uint32_t        s_writes_done = 0, s_writes_dropped = 0, s_commits = 0;

static void nvs_apply(const set_write_t *w)
{
    switch (w->type) {
    case SV_U8:  nvs_set_u8 (s_nvs, w->key, w->v.u8);  break;
    case SV_U32: nvs_set_u32(s_nvs, w->key, w->v.u32); break;
    case SV_I32: nvs_set_i32(s_nvs, w->key, w->v.i32); break;
    }
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

                for (int i = 0; i < n_pending; i++) nvs_apply(&pending[i]);
                nvs_commit(s_nvs);
                s_writes_done += n_pending;
                s_commits++;
                n_pending = 0;
                pending[n_pending++] = w;
            }
        }

        for (int i = 0; i < n_pending; i++) nvs_apply(&pending[i]);
        nvs_commit(s_nvs);
        s_writes_done += n_pending;
        s_commits++;
    }
}

static void set_put(const char *key, sv_type_t type, uint32_t raw)
{
    if (!s_nvs_ok || !key || !*key) return;

    set_write_t w;
    memset(&w, 0, sizeof(w));
    strlcpy(w.key, key, sizeof(w.key));
    w.type = type;
    switch (type) {
    case SV_U8:  w.v.u8  = (uint8_t)raw;  break;
    case SV_U32: w.v.u32 = raw;           break;
    case SV_I32: w.v.i32 = (int32_t)raw;  break;
    }

    if (!s_wq) {

        nvs_apply(&w);
        nvs_commit(s_nvs);
        return;
    }

    if (xQueueSend(s_wq, &w, 0) != pdTRUE) s_writes_dropped++;
}

static inline void sput_u8 (const char *k, uint8_t v)  { set_put(k, SV_U8,  v); }
static inline void sput_u32(const char *k, uint32_t v) { set_put(k, SV_U32, v); }
static inline void sput_i32(const char *k, int32_t v)  { set_put(k, SV_I32, (uint32_t)v); }

void settings_write_stats(uint32_t *done, uint32_t *dropped, uint32_t *commits)
{
    if (done)     *done     = s_writes_done;
    if (dropped)  *dropped  = s_writes_dropped;
    if (commits)  *commits  = s_commits;
}

bool settings_init(void)
{
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

    s_wq = xQueueCreateStatic(SET_Q_DEPTH, sizeof(set_write_t),
                              s_wq_store, &s_wq_ctrl);
    if (s_wq) {
        xTaskCreateStatic(set_worker, "settings_wr", SET_STACK_WORDS, NULL, 2,
                          s_worker_stack, &s_worker_tcb);
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
    if (!s_nvs_ok) return false;
    int32_t ilat = 0, ilon = 0;
    if (nvs_get_i32(s_nvs, "home_lat", &ilat) != ESP_OK) return false;
    if (nvs_get_i32(s_nvs, "home_lon", &ilon) != ESP_OK) return false;
    if (ilat == 0 && ilon == 0) return false;
    if (lat) *lat = (float)ilat / 1000000.0f;
    if (lon) *lon = (float)ilon / 1000000.0f;
    return true;
}
void settings_set_home(float lat, float lon)
{
    if (!s_nvs_ok) return;
    sput_i32("home_lat", (int32_t)(lat * 1000000.0f));
    sput_i32("home_lon", (int32_t)(lon * 1000000.0f));
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
