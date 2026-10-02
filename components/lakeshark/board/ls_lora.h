/* The LoRa-socket radio on the base board: an SX1262 today, or an LR20xx
   (LR2021 class) part. ls_lora.c is a dispatcher; the chip is found at
   ls_lora_start() and everything below goes to whichever backend bound. */

#ifndef LS_LORA_H
#define LS_LORA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which part answered. Only ls_lora_start() sets this; code that cares what a
   radio CAN DO should ask ls_lora_caps(), and the chip kind is for the
   console and diagnostics. */
typedef enum {
    LS_LORA_CHIP_NONE = 0,
    LS_LORA_CHIP_SX126X,
    LS_LORA_CHIP_LR20XX,
} ls_lora_chip_t;

/* What the bound driver implements today, not what the silicon could do
   (except the two silicon facts marked below). A bit that is clear means the
   matching calls return ESP_ERR_NOT_SUPPORTED. */
#define LS_LORA_CAP_LORA        (1u << 0)  /* LoRa packets: configure/receive/send/poll */
#define LS_LORA_CAP_FSK         (1u << 1)  /* ls_lora_fsk_* sessions                    */
#define LS_LORA_CAP_OOK_RX      (1u << 2)  /* OOK receive                               */
#define LS_LORA_CAP_MODES_RX    (1u << 3)  /* ls_lora_modes_*: receive-only Mode S      */
#define LS_LORA_CAP_WIDE_RX_BW  (1u << 4)  /* silicon: channel filter of 2 MHz or more  */
#define LS_LORA_CAP_RSSI_INST   (1u << 5)  /* ls_lora_rssi_inst while receiving         */
#define LS_LORA_CAP_BAND_1G5_2G5 (1u << 6) /* silicon: 1.5-2.5 GHz receive input        */
#define LS_LORA_CAP_RX_WIDE     (1u << 7)  /* sweeps and FSK listening past 960 MHz    */

/* Where a receive-only sweep or an FSK listener may sit. Without
   LS_LORA_CAP_RX_WIDE that is 150-960 MHz, the range every sweep has always
   had. With it, 150-1100 MHz on the LF input, and with
   LS_LORA_CAP_BAND_1G5_2G5 as well, 1500-2500 MHz on the HF input. A sweep
   stays on one input: a band that crosses from one to the other is refused,
   because the gap between them is not a band the part can tune. Nothing in
   these ranges is a transmit permission; transmitting stops at 960 MHz
   whatever the part. */
#define LS_LORA_RX_MIN_HZ       150000000u
#define LS_LORA_RX_NARROW_MAX_HZ 960000000u
#define LS_LORA_RX_LF_MAX_HZ    1100000000u
#define LS_LORA_RX_HF_MIN_HZ    1500000000u
#define LS_LORA_RX_HF_MAX_HZ    2500000000u

/* Whether [min_hz, max_hz] is a range the radio with capabilities `caps`
   can sweep or listen across, as above. min_hz == max_hz asks about one
   frequency. Pure. */
static inline bool ls_lora_rx_range_ok(uint32_t caps, uint32_t min_hz, uint32_t max_hz)
{
    if (max_hz < min_hz || min_hz < LS_LORA_RX_MIN_HZ) return false;
    if (!(caps & LS_LORA_CAP_RX_WIDE)) return max_hz <= LS_LORA_RX_NARROW_MAX_HZ;
    if (max_hz <= LS_LORA_RX_LF_MAX_HZ) return true;
    return (caps & LS_LORA_CAP_BAND_1G5_2G5) &&
           min_hz >= LS_LORA_RX_HF_MIN_HZ && max_hz <= LS_LORA_RX_HF_MAX_HZ;
}

ls_lora_chip_t ls_lora_chip(void);
const char    *ls_lora_chip_name(void);   /* "none", "SX126x", "LR2021", ... */
uint32_t       ls_lora_caps(void);        /* 0 until a part has answered      */

/* Release reset, wait for the part, and identify it: an LR20xx first (GetStatus
   then GetVersion), then an SX126x. Returns ESP_ERR_NOT_FOUND when nothing
   answers as either, which includes all ones or all zeros on MISO (an absent
   or unpowered part on a bus that is otherwise healthy) and a status that is
   not an SX126x status. A part is never reported as an SX1262 unless it
   answered like one. */
esp_err_t ls_lora_start(void);
/* True when a part that can be used as a LoRa radio answered: an SX126x or an
   LR20xx (both have LS_LORA_CAP_LORA). An LR20xx transmits only below 1 GHz,
   on its LF path; anything at or above that is ESP_ERR_NOT_SUPPORTED. */
bool      ls_lora_present(void);

/* Forget the part. ls_lora_start() returns early once it has succeeded, so
   without this a radio that browns out, resets, or is powered down by the
   expander stays "present" forever and every later start is a no-op that
   reports success. Call this when the part's power or reset is touched by
   anything other than this driver. */
void      ls_lora_stop(void);

/* Raw status byte from GetStatus. SX126x: bits 6:4 are the chip mode, 3:1 the
   last command status; both being 0 or both being 7 means nothing is
   answering. LR20xx: Stat(7:0), bits 7:4 reset source, 2:0 chip mode. */
esp_err_t ls_lora_status(uint8_t *out);

/* SX126x only; ESP_ERR_NOT_SUPPORTED on an LR20xx. */
esp_err_t ls_lora_read_reg(uint16_t addr, uint8_t *buf, size_t len);

/* An FSK session. The caller must hold the MeshCore radio. */
typedef struct {
    uint32_t freq_hz;
    uint32_t bitrate;
    uint32_t deviation_hz;
    uint32_t bandwidth_hz;
    uint32_t sync_word;
    uint8_t payload_bytes;
    /* Preamble length in BITS. 0 means the 32 this session sent before the
       field existed, so a caller that only listens can leave it alone. A
       protocol whose receiver opens on preamble has to say what it wants -
       eight bytes of it is 64 - because the radio transmits exactly what it
       is told, and a preamble shorter than the far end waits for is a frame
       nobody ever hears. */
    uint16_t preamble_bits;
    /* Only read when the session transmits. -9..22 dBm, and 0 is a real
       level rather than a sentinel, so a listener's zeroed struct simply
       configures a power it never uses. */
    int8_t power_dbm;
    /* How much of sync_word the part must match, in bits. 0 means the 32
       this session always used, and 32 is also the most it will take: the
       part itself will match 64, but sync_word is a uint32_t and only four
       registers are written from it, so anything longer matches against a
       register nobody set.

       Short values are for discovery rather than reception: a receiver
       cannot be told to find an unknown sync word, because matching one is
       the only thing it does with it. Triggering on eight bits of the
       preamble instead puts whatever follows the preamble - which includes
       the real sync word - into the buffer where it can be read. */
    uint8_t sync_bits;
    /* LR20xx: poll returns the packet status RSSI latched at sync rather
       than its average packet RSSI. Other backends return NOT_SUPPORTED
       when this receive diagnostic is requested. */
    bool rssi_at_sync;
} ls_fsk_cfg_t;

/* The nearest receive bandwidth the part actually has, at or above `hz`.

   The filter is a ladder of fixed rungs, not a continuous setting, and a
   value between two of them is refused outright - which surfaces as a
   session that will not start, with nothing on screen saying why. Anything
   choosing a bandwidth from a rate and a deviation should come through here
   first. The ladder is the SX126x's, or the LR20xx's own while one is bound;
   an LR20xx session also takes an in-between value as the rung above it. */
uint32_t ls_lora_fsk_bw_snap(uint32_t hz);

esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg);
int       ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi_dbm);

/* Transmit one frame inside the running session and return immediately; poll
   ls_lora_send_done() for the end of it.

   The preamble and the sync word belong to the session and the radio emits
   them itself, so `data` is only what follows them, not the whole frame.
   Handing over bytes the sync word already covers puts them on air twice.

   The part is left in standby afterwards; ls_lora_fsk_receive() listens
   again. */
esp_err_t ls_lora_fsk_send(const uint8_t *data, size_t len);

/* Re-arm continuous receive, restoring the session's payload length. */
esp_err_t ls_lora_fsk_receive(void);

/* Move a running FSK session to `freq_hz` and listen there, keeping every
   other setting: for a listener that hops between a few channels, where
   ending the session and beginning another would hand the part back to LoRa
   in between. The new frequency must be on the same input as the old one
   (ls_lora_rx_range_ok). ESP_ERR_INVALID_STATE with no session or during a
   transmit, ESP_ERR_INVALID_ARG for a frequency it cannot reach, and
   ESP_ERR_NOT_SUPPORTED on a part without it (the SX126x). */
esp_err_t ls_lora_fsk_retune(uint32_t freq_hz);

esp_err_t ls_lora_fsk_end(void);
bool      ls_lora_fsk_active(void);

/* ---- Mode S (1090 MHz) frame source ------------------------------------ */

/* A receive-only session that hands over Mode S frames the part found itself,
   as the 224 OOK chips (28 bytes, MSB first, two chips per bit) that follow the
   preamble. There is no magnitude stream behind it, only frames. Needs
   LS_LORA_CAP_MODES_RX; every call is ESP_ERR_NOT_SUPPORTED without it. The
   caller must hold the MeshCore radio, as for an FSK session, and nothing in
   this session can transmit.

   freq_hz is 1080..1100 MHz. gain_step is 0 for the part's AGC, or 1..13 for a
   fixed gain with 13 the most. The part is left in standby if begin fails. */
#define LS_LORA_MODES_FRAME_BYTES 28
#define LS_LORA_MODES_GAIN_MAX    13

esp_err_t ls_lora_modes_begin(uint32_t freq_hz, int gain_step);
/* Non-blocking. LS_LORA_MODES_FRAME_BYTES with a frame in `buf` (`size` at
   least that), 0 when none is waiting, negative on a bus or protocol failure
   the session did not recover from. `rssi_dbm`, which may be NULL, gets the
   level of the frame's high chips, or NAN when it is not known. Call it from a
   task about once a millisecond: the part holds nine frames. */
int       ls_lora_modes_poll(uint8_t *buf, size_t size, float *rssi_dbm);
esp_err_t ls_lora_modes_set_gain(int gain_step);

/* Live tuning of a running Mode S session, for working out why a receiver
   triggers on noise. Runtime only: nothing is stored, and the next
   ls_lora_modes_begin starts from the defaults again (the gain asked for,
   boost 7, the widest filter, the part's own detection threshold). Each call
   is under the socket's lock: the part goes to standby, the one setting is
   reprogrammed, and Rx is armed again, with no other task's command in
   between. ESP_ERR_INVALID_STATE with no session, ESP_ERR_NOT_SUPPORTED
   without LS_LORA_CAP_MODES_RX. Setting what is already in force sends
   nothing. */
typedef struct {
    int      gain_step;        /* 0 is the part's AGC, 1..13 fixed               */
    int      boost;            /* receive boost 0..7                             */
    uint32_t rx_bw_hz;         /* the filter rung in use                         */
    bool     thresh_override;  /* a detection threshold of ours is in force      */
    int      thresh_level;     /* its level in dB, when thresh_override          */
} ls_lora_modes_tuning_t;
esp_err_t ls_lora_modes_tuning(ls_lora_modes_tuning_t *out);
esp_err_t ls_lora_modes_set_boost(int boost);                  /* 0..7 */
/* The filter rung nearest `hz`; `chosen_hz` (may be NULL) says which. */
esp_err_t ls_lora_modes_set_bw(uint32_t hz, uint32_t *chosen_hz);
/* Override the part's automatic OOK detection threshold, level_db -64..63, or
   hand it back to the part with automatic = true. */
esp_err_t ls_lora_modes_set_threshold(int level_db, bool automatic);
/* The threshold field as the part holds it now, raw 0..127 (level = raw - 64),
   read over SPI. */
esp_err_t ls_lora_modes_read_threshold(int *raw);

/* Back to standby with the part's interrupts and FIFO cleared. */
esp_err_t ls_lora_modes_end(void);
bool      ls_lora_modes_active(void);

/* ---- OOK receive session, any pulse train --------------------------------- */

/* The Mode S session's machinery with the numbers left open: OOK at a bit
   rate of the caller's choosing, the part's detector looking for `pattern`
   (bit 0 is the first on air, as Mode S's 0x0285 is its preamble), and then
   `frame_bytes` of decisions handed over, MSB first, one per bit period.
   Run faster than the pulses it is listening for, it is a sampler: what comes
   out is the on/off waveform at a fixed rate. Receive-only, like Mode S, and
   the same hardware: it needs LS_LORA_CAP_MODES_RX, and it and a Mode S
   session cannot run at once. The caller must hold the MeshCore radio. */
#define LS_LORA_OOK_FRAME_MAX 252

typedef struct {
    uint32_t freq_hz;       /* 150..1213 MHz LF; >1100 sensitivity unverified  */
    uint32_t bitrate;       /* decisions a second, 600..2000000               */
    uint32_t bandwidth_hz;  /* the narrowest filter rung at or above this     */
    uint16_t pattern;       /* detector pattern, bit 0 first on air           */
    uint8_t  pattern_bits;  /* 1..16                                          */
    uint8_t  repeats;       /* how many more times the pattern must come, 0..31 */
    uint8_t  frame_bytes;   /* 1..LS_LORA_OOK_FRAME_MAX                       */
    uint8_t  gain_step;     /* 0 the part's AGC, 1..13 fixed                  */
    int8_t   boost;         /* receive boost 0..7, or -1 for the part's default */
} ls_ook_cfg_t;

esp_err_t ls_lora_ook_begin(const ls_ook_cfg_t *cfg);
/* Non-blocking. frame_bytes with a frame in `buf`, 0 when none is waiting,
   negative on a failure the session did not recover from. `rssi_dbm` (may be
   NULL) gets the level of the frame's high decisions, or NAN. */
int       ls_lora_ook_poll(uint8_t *buf, size_t size, float *rssi_dbm);
esp_err_t ls_lora_ook_end(void);
bool      ls_lora_ook_active(void);

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

   On an SX126x this is the function that reads DIO1 over the expander, so it
   carries the ~10 ms latency floor the file header describes; an LR20xx is
   asked over SPI instead, one GetStatus a call. Call it from a task, never
   from an ISR - it does I2C or SPI. */
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

/* Advance a bounded batch (20ms budget, checked between bins).
   Returns n while the plan is progressing, 0 on failure. Only row_done
   declares all bins/looks complete. Keep the same buffer until then.
   The first look replaces values; subsequent looks retain each bin's peak. */
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

/* How long after SetRx a reading through a `bw_hz` filter is believed: the
   measured 300 us at 500 kHz, held for the same number of filter samples as
   the filter narrows, and no shorter for a wider one. Pure. */
uint32_t ls_lora_scan_settle_us(uint32_t bw_hz);

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
