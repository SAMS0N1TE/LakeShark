#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "app_registry.h"
#include "p25_cqpsk_controls.h"

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
/* RAM-only read; setters report acceptance by the deferred write queue. */
bool settings_set_home(float lat, float lon);
bool settings_clear_home(void);

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

/**/
bool settings_get_usb_autoreboot(void);
void settings_set_usb_autoreboot(bool en);

/* Which antenna the SKY13453 is pointing at, across reboots. */

bool settings_get_antenna_external(void);
void settings_set_antenna_external(bool external);

bool settings_get_alert_ring(void);
void settings_set_alert_ring(bool en);
bool settings_get_alert_vibe(void);
void settings_set_alert_vibe(bool en);

/* Turn the screen to match the way the board is being held.

   Defaults ON, because a handheld that has to be told which way up it is
   through a key on a DETACHABLE keyboard is the thing this replaces. Off is
   one tap for anybody reading in bed. */
bool settings_get_auto_rotate(void);
void settings_set_auto_rotate(bool enabled);

/**/
bool settings_get_nav_autohide(void);
void settings_set_nav_autohide(bool enabled);
int settings_get_theme(void);
void settings_set_theme(int theme);

bool settings_get_daylight(void);
void settings_set_daylight(bool on);

/* HOME widget IDs are defined in home_widget_pref.h. Getter is RAM-only;
 * false means selection was not accepted by the deferred persistence path. */
int  settings_get_home_widget(void);
bool settings_set_home_widget(int widget);

/**/
int  settings_get_scan_zone(void);
void settings_set_scan_zone(int zone);

int  settings_get_p25_demod(void);
void settings_set_p25_demod(int mode_idx);

/* P25 traffic/encryption controls. Setters report whether the value
 * reached the safe settings path, allowing CONFIG to make a full/absent write
 * queue visible instead of silently claiming persistence. */
bool     settings_get_p25_auto_follow(void);
bool     settings_set_p25_auto_follow(bool enabled);
bool     settings_get_p25_skip_encrypted(void);
bool     settings_set_p25_skip_encrypted(bool enabled);
uint32_t settings_get_p25_encrypted_skip_ms(void);
bool     settings_set_p25_encrypted_skip_ms(uint32_t ms);
void     settings_get_p25_cqpsk(p25_cqpsk_config_t *config);
bool     settings_set_p25_cqpsk(const p25_cqpsk_config_t *config);

/**/
void settings_reset_app(const app_t *a);

int  settings_voice_preset_get(void);
void settings_voice_preset_set(int preset);
int  settings_voice_lowpass_get(void);
void settings_voice_lowpass_set(int mode);
int  settings_voice_lowshelf_get(void);
void settings_voice_lowshelf_set(int mode);

int  settings_eq_preset_get(void);
void settings_eq_preset_set(int v);
int  settings_eq_hp_get(void);
void settings_eq_hp_set(int v);
int  settings_eq_bass_get(void);
void settings_eq_bass_set(int v);
int  settings_eq_treb_get(void);
void settings_eq_treb_set(int v);
int  settings_eq_punch_get(void);
void settings_eq_punch_set(int v);
int  settings_eq_loud_get(void);
void settings_eq_loud_set(int v);

void settings_write_stats(uint32_t *done, uint32_t *dropped, uint32_t *commits);

#ifdef __cplusplus
}
#endif

#endif
