
#ifndef FM_STATE_H
#define FM_STATE_H

#include <stdint.h>
#include <stdbool.h>

#include "iq_app_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FM_RTL_RATE     256000
#define FM_DEMOD_RATE   32000
#define FM_AUDIO_RATE   16000

#define FM_DEFAULT_FREQ  152600000UL

#define FM_FREQ_LISTEN   154785000UL
#define FM_FREQ_POCSAG   152600000UL
#define FM_FREQ_FLEX     152600000UL
#define FM_FREQ_WFM       91000000UL
/* ACARS air-band datalink - 131.550 MHz is the primary North American
   channel; the AppACARS panel exposes 130.025 and 129.125 too. */
#define FM_FREQ_ACARS    131550000UL
#define FM_DEFAULT_GAIN  200

typedef enum {
    FM_MODE_LISTEN = 0,
    FM_MODE_SCAN   = 1,
    FM_MODE_POCSAG = 2,
    FM_MODE_WFM    = 3,
    FM_MODE_ACARS  = 4,
    FM_MODE_FLEX   = 5,
    /* Value 6 was a removed decoder. Do not reuse persisted mode IDs. */
    FM_MODE_COUNT = 6
} fm_mode_t;

/**/

#define FM_SCAN_BINS_MAX  256

/* The scale scan_db[] is written on, published. */

#define FM_SCAN_FLOOR_DB  (-90.0f)
#define FM_SCAN_TOP_DB    (0.0f)

#define FM_PAGE_TEXT_MAX  80
#define FM_PAGE_LOG_MAX   16

typedef enum {
    FM_PAGE_PROTOCOL_POCSAG = 0,
    FM_PAGE_PROTOCOL_FLEX   = 1,
} fm_page_protocol_t;

typedef struct {
    int64_t  ts_us;
    /* Wall-clock UNIX epoch at the moment the page was decoded, or 0
       if no wall clock had been set yet. Uptime in ts_us is always present
       for ordering within one boot; ts_epoch is what lets a page be tied
       to real calendar time when SNTP/RTC has landed. */
    int64_t  ts_epoch;
    uint32_t address;
    uint8_t  function;
    fm_page_protocol_t protocol;
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
    /**/
    /* scan_idx is now a TUNE POSITION, not a display bin - one tune paints
       FM_SPEC_USABLE_HZ worth of bins at once. scan_tunes is how many tune
       positions cover the range. */
    int       scan_tunes;
    int       scan_idx;
    float     scan_db[FM_SCAN_BINS_MAX];
    uint32_t  scan_peak_hz;
    float     scan_peak_db;
    uint32_t  scan_sweeps;
    /* How long one sweep takes, in ms: estimated from the tune count
       when a sweep begins, measured once a whole one has run. The waterfall
       needs it to tell a slow sweep from a stalled one. */
    uint32_t  scan_sweep_ms;

    int       pocsag_baud;
    int       pocsag_lock_baud;
    bool      pocsag_auto;
    bool      pocsag_sync;
    uint32_t  pocsag_frames;
    uint32_t  pocsag_pages;
    uint32_t  pocsag_cw_errs;
    uint32_t  pocsag_addr;
    uint32_t  pocsag_msg;

    bool      flex_sync;
    uint8_t   flex_mode;
    uint32_t  flex_frames;
    uint32_t  flex_pages;
    uint32_t  flex_cw_errs;
    int       flex_near_min;

    int       page_head;
    int       page_count;
    fm_page_t pages[FM_PAGE_LOG_MAX];
} fm_state_t;

extern fm_state_t FM;

/* Requested values remain in FM.freq_hz/FM.gain_tenths for compatibility.
   This snapshot is the authoritative receiver-side result. */
void fm_get_receiver_status(ls_iq_control_status_t *out);

#ifdef __cplusplus
}
#endif

#endif
