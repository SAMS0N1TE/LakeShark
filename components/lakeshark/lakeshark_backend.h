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
void lakeshark_select_rec(void);

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

void        lakeshark_radio_recover(void);

void        lakeshark_set_usb_autoreboot(bool en);
bool        lakeshark_usb_autoreboot(void);

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

bool        lakeshark_p25_agc_enabled(void);

typedef struct {
    uint32_t freq_hz;
    int      demod_mode;
    int      gain_tenths;
    int      agc_on;
    int      nac;
    int      tg;
    int      src;
    int      has_sync;
    int      voice_active;
    int      sync_count;
    int      voice_count;
    int      bch_ok;
    int      bch_fail;
    int      iq_level;
    int      polarity_inverted;
    int      beep;
    int      voice_gate;
    int      rtl_ready;
    int      ring_fill;
    int      ring_size;
    int      read_errors;
    uint32_t iq_bytes_sec;
    uint32_t audio_drops;
    int      decode_us;

    int      nac_age_ms;
    int      tg_age_ms;
    int      src_age_ms;
    char     ftype[16];
    char     err[64];
} lakeshark_p25_tel_t;

typedef struct {
    int      submode;
    uint32_t freq_hz;
    int      gain_tenths;
    int      iq_level;
    int      audio_level;
    int      squelch_tenths;
    int      squelch_open;
    uint32_t iq_bytes_sec;
    int      read_errors;

    uint32_t scan_start_hz;
    uint32_t scan_stop_hz;
    uint32_t scan_peak_hz;
    int      scan_peak_db;
    uint32_t scan_sweeps;

    int      pocsag_baud;
    int      pocsag_auto;
    int      pocsag_sync;
    uint32_t pocsag_pages;
    uint32_t pocsag_frames;
    uint32_t pocsag_last_addr;
    int      pocsag_last_baud;
    char     pocsag_last_type;
    char     pocsag_last_text[80];
} lakeshark_fm_tel_t;

void        lakeshark_fm_telemetry(lakeshark_fm_tel_t *out);

void        lakeshark_p25_telemetry(lakeshark_p25_tel_t *out);

#define LAKESHARK_ADSB_MAX 16

typedef struct {
    uint32_t icao;
    char     callsign[9];
    int      altitude;
    int      velocity;
    int      heading;
    int      vert_rate;
    float    lat;
    float    lon;
    int      pos_valid;
    int      msg_count;
    int      age_ms;
} lakeshark_adsb_ac_t;

typedef struct {
    uint32_t freq_hz;
    int      gain_tenths;
    int      rtl_ready;
    uint32_t iq_bytes_sec;

    int      tracked;
    int      msgs_total;
    int      msgs_sec;
    int      crc_good;
    int      crc_err;
    int      bursts_sec;
    int      mag_avg;
    int      mag_peak;
    int      last_msg_ms;

    int      n_aircraft;
} lakeshark_adsb_tel_t;

void        lakeshark_adsb_telemetry(lakeshark_adsb_tel_t *out);

bool        lakeshark_adsb_aircraft_at(int index, lakeshark_adsb_ac_t *out);

void        lakeshark_cartotui_set_enabled(bool en);
bool        lakeshark_cartotui_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
