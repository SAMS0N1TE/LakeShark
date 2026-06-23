
#include "settings.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <ctype.h>
#include <string.h>

static const char  *TAG      = "settings";
static const char  *NS       = "sdr-tool";
static nvs_handle_t s_nvs    = 0;
static bool         s_nvs_ok = false;

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
    nvs_set_u32(s_nvs, k, hz);
    nvs_commit(s_nvs);
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
    nvs_set_u32(s_nvs, k, hz);
    nvs_commit(s_nvs);
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
    nvs_set_i32(s_nvs, k, (int32_t)tenths);
    nvs_commit(s_nvs);
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
    nvs_set_u32(s_nvs, k, hz);
    nvs_commit(s_nvs);
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
    nvs_set_i32(s_nvs, "home_lat", (int32_t)(lat * 1000000.0f));
    nvs_set_i32(s_nvs, "home_lon", (int32_t)(lon * 1000000.0f));
    nvs_commit(s_nvs);
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
    nvs_set_u8(s_nvs, "brightness", (uint8_t)pct);
    nvs_commit(s_nvs);
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
    nvs_set_u8(s_nvs, "autodim", en ? 1 : 0);
    nvs_commit(s_nvs);
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
    nvs_set_u8(s_nvs, "autodim_to", (uint8_t)seconds);
    nvs_commit(s_nvs);
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
    nvs_set_u8(s_nvs, "volume", (uint8_t)pct);
    nvs_commit(s_nvs);
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
    nvs_set_u8(s_nvs, "boot_snd", (uint8_t)mode);
    nvs_commit(s_nvs);
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
    nvs_set_u8(s_nvs, "voice_preset", (uint8_t)p);
    nvs_commit(s_nvs);
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
    nvs_set_u8(s_nvs, "voice_lp", (uint8_t)m);
    nvs_commit(s_nvs);
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
    nvs_set_u8(s_nvs, "voice_shelf", (uint8_t)m);
    nvs_commit(s_nvs);
}
