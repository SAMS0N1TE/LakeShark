/* settings.c - NVS persistence for per-app and voice settings. */

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

/* NVS caps key length at 15 chars including NUL. App names are lowercased
 * and stripped of '-', ' ', '_' before being concatenated with the field. */
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
