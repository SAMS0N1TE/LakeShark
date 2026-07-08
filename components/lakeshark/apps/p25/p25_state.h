#ifndef P25_STATE_H
#define P25_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "dsd.h"
#include "dsp_pipeline.h"

#ifndef __RTL_SDR_H
typedef struct rtlsdr_dev rtlsdr_dev_t;
#endif

#define RTL_SAMPLE_RATE       240000
#define RTL_DEFAULT_GAIN      200

#define P25_USB_PACKET_SIZE   8192
#define P25_USB_BUF_LENGTH    (P25_USB_PACKET_SIZE * 2)

#define SCAN_BINS_MAX         60
#define SCAN_IQ_SAMPLES       4096
#define SCAN_WATERFALL_ROWS   8

typedef struct {
    uint32_t start_freq;
    uint32_t stop_freq;
    uint32_t step_hz;
    int      num_bins;
    float    power[SCAN_BINS_MAX];
    float    waterfall[SCAN_WATERFALL_ROWS][SCAN_BINS_MAX];
    int      wf_row;
    int      peak_bin;
    float    peak_power;
    uint32_t peak_freq;
    float    noise_floor;
    int      sweep_count;
    bool     scanning;
    bool     request_scan;
    bool     request_tune;
} scan_state_t;

typedef struct {
    uint32_t iq_bytes_total;
    uint32_t iq_bytes_sec;
    uint32_t audio_samples_sec;
    int      ring_fill;
    int      ring_size;
    int      read_errors;
    float    iq_level;

    int      dsd_sync_count;
    int      dsd_voice_count;
    int      dsd_nac;
    int      dsd_tg;
    int      dsd_src;
    char     dsd_ftype[16];
    char     dsd_fsubtype[16];
    char     dsd_modulation[8];
    char     dsd_err_str[64];
    bool     dsd_has_sync;
    bool     dsd_buffers_ok;
    int      dsd_bch_ok_count;
    int      dsd_bch_fail_count;
    int      dsd_last_ok_nac;
    char     dsd_last_ok_duid[4];
    int      dsd_enc;

    int64_t  voice_active_until_us;

    int64_t  sync_active_until_us;

    float    demod_gain;
    bool     demod_invert;
    int      rtl_gain_tenths;

    float    dsd_decode_ms;
    uint32_t audio_drops;

    bool     autoscan_active;
    bool     autoscan_locked;
    int      autoscan_bch_ok;
    int      autoscan_step;

    int      input_mode;
    char     input_buf[16];
    int      input_pos;

    bool     sync_beep_enabled;
} p25_state_t;

extern p25_state_t      P25;
extern scan_state_t     SCAN;
extern uint32_t         s_tune_freq_hz;
extern volatile uint32_t s_p25_freq_req;
extern rtlsdr_dev_t    *rtldev;
extern dsp_state_t      s_dsp;
extern dsd_opts         s_dsd_opts;
extern dsd_state        s_dsd_state;
extern dsd_sample_ring_t s_ring;

void sys_log(uint8_t color, const char *fmt, ...);

#endif
