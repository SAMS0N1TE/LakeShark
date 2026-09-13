/* SX1262 LoRa radio on the base board. */

#ifndef LS_LORA_H
#define LS_LORA_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Release reset, wait for the part, and confirm it answers. Returns
   ESP_ERR_NOT_FOUND when the status read comes back as all ones or all
   zeros, which is what an absent or unpowered part looks like on a bus that
   is otherwise healthy. */
esp_err_t ls_lora_start(void);
bool      ls_lora_present(void);

/* Forget the part. ls_lora_start() returns early once it has succeeded, so
   without this a radio that browns out, resets, or is powered down by the
   expander stays "present" forever and every later start is a no-op that
   reports success. Call this when the part's power or reset is touched by
   anything other than this driver. */
void      ls_lora_stop(void);

/* Raw status byte from GetStatus. Bits 6:4 are the chip mode, 3:1 the last
   command status; both being 0 or both being 7 means nothing is answering. */
esp_err_t ls_lora_status(uint8_t *out);

esp_err_t ls_lora_read_reg(uint16_t addr, uint8_t *buf, size_t len);

/* Receive-only FSK session. The caller must hold the MeshCore radio. */
typedef struct {
    uint32_t freq_hz;
    uint32_t bitrate;
    uint32_t deviation_hz;
    uint32_t bandwidth_hz;
    uint32_t sync_word;
    uint8_t payload_bytes;
} ls_fsk_cfg_t;

esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg);
int       ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi_dbm);
esp_err_t ls_lora_fsk_end(void);
bool      ls_lora_fsk_active(void);

/* ---- packet path ------------------------------------------------------- */

typedef struct {
    uint32_t freq_hz;
    uint8_t  sf;          /* 5..12                                          */
    uint32_t bw_hz;       /* 7810..500000, snapped to the chip's ladder     */
    uint8_t  cr;          /* 5..8, meaning 4/5..4/8                         */
    int8_t   power_dbm;   /* -9..22                                         */
    uint16_t preamble;    /* symbols                                        */
    uint8_t  sync_word;   /* 0x12 private, 0x34 public                      */
    bool     crc_on;
    bool     invert_iq;
    uint16_t cal_min_mhz; /* image calibration band, 902/928 for US915      */
    uint16_t cal_max_mhz;
} ls_lora_cfg_t;

/* The defaults this board ships with, US915. Fill a struct from this and
   change only what you mean to change. */
void      ls_lora_cfg_default(ls_lora_cfg_t *out);

/* Standby, calibrate, set modulation and packet parameters, then park in
   standby. Safe to call again to retune. */
esp_err_t ls_lora_configure(const ls_lora_cfg_t *cfg);
const ls_lora_cfg_t *ls_lora_cfg(void);

/* Put the part in continuous receive. Idempotent. */
esp_err_t ls_lora_receive(void);
bool      ls_lora_is_receiving(void);

/* Start a transmit; returns immediately. ESP_ERR_INVALID_STATE if a transmit
   is already in flight, ESP_ERR_INVALID_SIZE above 255 bytes. */
esp_err_t ls_lora_send(const uint8_t *data, size_t len);

/* True once the in-flight transmit has finished (or timed out). Poll it. */
bool      ls_lora_send_done(void);

/* Non-blocking receive. Returns the packet length, 0 when nothing arrived,
   negative on a CRC or header error. `rssi_dbm` and `snr_db` are filled on a
   good packet and may be NULL.

   This is the function that reads DIO1 over the expander, so it carries the
   ~10 ms latency floor the file header describes. Call it from a task, never
   from an ISR - it does I2C. */
int       ls_lora_poll(uint8_t *buf, size_t max, float *rssi_dbm, float *snr_db);

/* Instantaneous channel RSSI while receiving, for a noise floor estimate. */
esp_err_t ls_lora_rssi_inst(float *dbm);

/* Air time in ms for a payload of `len` under the current configuration.
   The Semtech time-on-air formula, not an approximation: a duty-cycle budget
   computed from a guess is a licence violation waiting to happen. */
uint32_t  ls_lora_airtime_ms(int len);

/* THE RADIO AS AN INSTRUMENT, not only as a modem. */

/* Take the radio for a sweep of [min_hz, max_hz]. */

esp_err_t ls_lora_scan_begin(uint32_t min_hz, uint32_t max_hz);

/* One complete row. Fills `n` bins with dBm, oldest business first: bin 0 is
   min_hz and bin n-1 is max_hz. Returns how many were filled. Takes every
   look the plan asks for before returning, so on a band wider than the
   filter can cover it costs that many passes - the console wants that; the
   waterfall uses ls_lora_scan_pass. */
int ls_lora_scan_sweep(float *dbm, int n);

/* One pass: one reading per bin, at this pass's look.

   The first look of a row overwrites `dbm`, every later one keeps the peak,
   so the caller hands the same buffer back until *row_done comes back true.
   With one look per row that is every pass. A pass costs the same whatever
   the band, which is what lets it run once a frame on the draw path. */
int ls_lora_scan_pass(float *dbm, int n, bool *row_done);

/* Give it back, restoring what was configured before the sweep began. */
esp_err_t ls_lora_scan_end(void);

bool ls_lora_scanning(void);

/* Where a sweep's time actually goes, in microseconds per bin.

   Added because the first measurement was 8.4 ms a bin against an estimate
   of 0.4, and an estimate that wrong is not fixed by a better estimate. Each
   field is the last pass's total for that phase divided by the bins in it,
   so the four of them add up to what the console prints as the per-bin cost
   and the largest one is the thing to work on. */
typedef struct {
    uint32_t standby_us;   /* SetStandby, including its BUSY wait   */
    uint32_t tune_us;      /* SetRfFrequency                        */
    uint32_t rx_us;        /* SetRx                                 */
    uint32_t settle_us;    /* the deliberate wait before reading    */
    uint32_t rssi_us;      /* GetRssiInst                           */
} ls_lora_scan_prof_t;

void ls_lora_scan_profile(ls_lora_scan_prof_t *out);

uint32_t ls_lora_scan_bin_hz(uint32_t min_hz, uint32_t max_hz, int n, int i);

/* How a band is swept: which filter, how long to wait before the
   RSSI is believed, and how many readings each bin is the peak of. Pure, and
   decided from the spacing between bin centres; ls_lora.c has the reasoning
   beside the arithmetic. */
typedef struct {
    uint32_t bw_hz;      /* receive bandwidth, exactly one rung of the ladder */
    uint32_t settle_us;  /* SetRx to a GetRssiInst worth believing           */
    int      looks;      /* readings per bin, spread across its slice        */
} ls_lora_scan_plan_t;

/* The bin count ls_lora_scan_begin plans for. The waterfall sweeps exactly
   this many, so it never pays for a second plan; a caller that asks for a
   different count gets one replan on its first pass. */
#define LS_LORA_SCAN_BINS       64

#define LS_LORA_SCAN_LOOKS_MAX  32

void ls_lora_scan_plan(uint32_t min_hz, uint32_t max_hz, int n,
                       ls_lora_scan_plan_t *out);

/* Where look `k` of `looks` for bin `i` is taken. With one look it
   is ls_lora_scan_bin_hz exactly; with more they tile the bin's slice, one
   filter width each, and never leave [min_hz, max_hz]. */
uint32_t ls_lora_scan_look_hz(uint32_t min_hz, uint32_t max_hz, int n, int i,
                              int looks, int k);

uint32_t ls_lora_freq_steps(uint32_t hz);

/* Console helper: bring it up if needed and describe what answered. */
void ls_lora_diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_LORA_H */
