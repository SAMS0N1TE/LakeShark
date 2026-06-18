
#ifndef FM_STATE_H
#define FM_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FM_RTL_RATE     256000
#define FM_DEMOD_RATE   32000
#define FM_AUDIO_RATE   16000

#define FM_DEFAULT_FREQ  152600000UL

#define FM_FREQ_LISTEN   154785000UL
#define FM_FREQ_POCSAG   152600000UL
#define FM_FREQ_WFM       91000000UL
#define FM_DEFAULT_GAIN  200

typedef enum {
    FM_MODE_LISTEN = 0,
    FM_MODE_SCAN   = 1,
    FM_MODE_POCSAG = 2,
    FM_MODE_WFM    = 3,
    FM_MODE_COUNT
} fm_mode_t;

#define FM_SCAN_BINS_MAX  128

#define FM_PAGE_TEXT_MAX  80
#define FM_PAGE_LOG_MAX   16

typedef struct {
    int64_t  ts_us;
    uint32_t address;
    uint8_t  function;
    char     type;
    uint16_t baud;
    char     text[FM_PAGE_TEXT_MAX];
} fm_page_t;

typedef struct {

    fm_mode_t mode;
    uint32_t  freq_hz;
    int       gain_tenths;
    float     iq_level;
    float     audio_level;
    bool      squelch_open;
    int       squelch_tenths;
    uint32_t  iq_bytes_sec;
    uint32_t  read_errors;

    uint32_t  scan_start_hz;
    uint32_t  scan_stop_hz;
    uint32_t  scan_step_hz;
    int       scan_bins;
    int       scan_idx;
    float     scan_db[FM_SCAN_BINS_MAX];
    uint32_t  scan_peak_hz;
    float     scan_peak_db;
    uint32_t  scan_sweeps;

    int       pocsag_baud;
    int       pocsag_lock_baud;
    bool      pocsag_auto;
    bool      pocsag_sync;
    uint32_t  pocsag_frames;
    uint32_t  pocsag_pages;
    uint32_t  pocsag_cw_errs;
    uint32_t  pocsag_addr;
    uint32_t  pocsag_msg;
    int       page_head;
    int       page_count;
    fm_page_t pages[FM_PAGE_LOG_MAX];
} fm_state_t;

extern fm_state_t FM;

#ifdef __cplusplus
}
#endif

#endif
