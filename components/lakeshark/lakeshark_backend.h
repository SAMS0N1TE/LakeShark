
#ifndef LAKESHARK_BACKEND_H
#define LAKESHARK_BACKEND_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void lakeshark_backend_start(void);
void lakeshark_boot_sound(void);

void lakeshark_select_adsb(void);
void lakeshark_select_p25(void);
void lakeshark_select_fm(void);

void     lakeshark_fm_set_mode(int mode);
int      lakeshark_fm_get_mode(void);
void     lakeshark_fm_tune(int delta_hz);
void     lakeshark_fm_set_freq(uint32_t hz);
uint32_t lakeshark_fm_get_freq(void);
void     lakeshark_fm_gain_step(void);
void     lakeshark_fm_agc(void);
void     lakeshark_fm_set_gain(int tenths);
void     lakeshark_fm_set_gain_live(int tenths);
void     lakeshark_fm_gain_delta(int dt);
int      lakeshark_fm_gain_tenths(void);
void     lakeshark_fm_set_baud(int baud);
int      lakeshark_fm_get_baud(void);
void     lakeshark_fm_squelch_delta(int d);
void     lakeshark_fm_set_squelch(int v);
int      lakeshark_fm_squelch_get(void);
void     lakeshark_fm_scan_restart(void);
void     lakeshark_fm_tune_to_peak(void);
uint32_t lakeshark_fm_scan_peak_hz(void);

void lakeshark_radio_park(void);
void lakeshark_radio_unpark(void);
bool lakeshark_radio_running(void);
bool lakeshark_radio_device_ready(void);
const char *lakeshark_recovery_take_app(void);

void lakeshark_radio_set_gain(int tenths);
void lakeshark_radio_set_gain_live(int tenths);
int  lakeshark_radio_get_gain_tenths(void);

void        lakeshark_p25_tune(int delta_hz);
void        lakeshark_p25_set_freq(uint32_t hz);
uint32_t    lakeshark_p25_get_freq(void);
const char *lakeshark_p25_cycle_mode(void);
const char *lakeshark_p25_mode_name(void);
int         lakeshark_p25_mode_index(void);
void        lakeshark_p25_set_mode(int idx);
void        lakeshark_p25_reset_stats(void);
void        lakeshark_p25_gain_step(void);
void        lakeshark_p25_agc(void);
bool        lakeshark_p25_agc_enabled(void);
int         lakeshark_p25_gain_tenths(void);
void        lakeshark_p25_beep_toggle(void);
bool        lakeshark_p25_beep_enabled(void);
void        lakeshark_p25_set_voice_gate(int v);
int         lakeshark_p25_voice_gate(void);
void        lakeshark_p25_toggle_polarity(void);
bool        lakeshark_p25_polarity_inverted(void);

void        lakeshark_adsb_gain_step(void);
void        lakeshark_adsb_agc(void);
int         lakeshark_adsb_gain_tenths(void);

void        lakeshark_cartotui_set_enabled(bool en);
bool        lakeshark_cartotui_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
