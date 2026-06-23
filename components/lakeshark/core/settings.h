
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "app_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_FAVOURITES 8

bool     settings_init(void);

uint32_t settings_get_freq(const app_t *a);
void     settings_set_freq(const app_t *a, uint32_t hz);

uint32_t settings_get_freq_mode(const app_t *a, int mode, uint32_t deflt);
void     settings_set_freq_mode(const app_t *a, int mode, uint32_t hz);
int      settings_get_gain(const app_t *a);
void     settings_set_gain(const app_t *a, int tenths);

int      settings_fav_count(const app_t *a);
uint32_t settings_fav_get(const app_t *a, int slot);
void     settings_fav_set(const app_t *a, int slot, uint32_t hz);
void     settings_fav_clear(const app_t *a, int slot);

bool settings_get_home(float *lat, float *lon);
void settings_set_home(float lat, float lon);

int  settings_get_brightness(void);
void settings_set_brightness(int pct);

bool settings_get_autodim(void);
void settings_set_autodim(bool en);
int  settings_get_autodim_timeout(void);
void settings_set_autodim_timeout(int seconds);

int  settings_get_volume(void);
void settings_set_volume(int pct);

int  settings_get_boot_sound(void);
void settings_set_boot_sound(int mode);

int  settings_voice_preset_get(void);
void settings_voice_preset_set(int preset);
int  settings_voice_lowpass_get(void);
void settings_voice_lowpass_set(int mode);
int  settings_voice_lowshelf_get(void);
void settings_voice_lowshelf_set(int mode);

#ifdef __cplusplus
}
#endif

#endif
