#ifndef ADSB_SOURCE_H
#define ADSB_SOURCE_H

/* Where ADS-B frames come from, and the loop that feeds the decoder from a
   frame source. Pure of hardware: the IQ path and the radio calls live in
   app_adsb.c, so this file is what the host bench exercises. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ADSB_SRC_NONE = 0,   /* nothing that can receive 1090 MHz            */
    ADSB_SRC_IQ,         /* an IQ receiver (RTL-SDR, HackRF): everything */
    ADSB_SRC_LORA,       /* the LoRa-socket chip's Mode S frame source   */
} adsb_source_t;

/* The receiver the RADIO picker's choice comes to. `want_lora` is that
   choice being the LoRa socket's part: it is taken when `lora_caps`
   (ls_lora_caps()) says the part has the Mode S session. Otherwise an IQ
   receiver when there is one - it feeds the magnitude, waterfall and phase
   work the chip has no stream for - and the chip after it. Neither is
   ADSB_SRC_NONE. The same order ls_rsel_effective falls back in, so the
   screen's RADIO button names what this picks. */
adsb_source_t adsb_source_choose(bool want_lora, bool iq_present, uint32_t lora_caps);

/* "none", "iq", "lr-chip": short enough for the receiver status line. */
const char *adsb_source_name(adsb_source_t source);

/* The app's gain setting (tenths of dB, 0..496, 0 meaning automatic) as the
   chip's gain step, 1..13 with 13 the most. Automatic is 13: the chip's AGC
   (step 0) is never chosen from the slider. */
int adsb_lr_gain_step(int gain_tenths_db);

/* A frame source as the receive loop sees it. ls_lora_modes_* fills it on
   the board; the bench fills it with a fake. poll() returns the bytes of one
   frame (28 chip bytes), 0 when there is none, negative on a failure. */
typedef struct {
    esp_err_t (*begin)(uint32_t freq_hz, int gain_step);
    int       (*poll)(uint8_t *buf, size_t size, float *rssi_dbm);
    esp_err_t (*set_gain)(int gain_step);
    esp_err_t (*end)(void);
} adsb_modes_ops_t;

#define ADSB_MODES_FRAME_BYTES 28

typedef struct {
    uint32_t frames;       /* frames taken from the source                    */
    uint32_t decoded;      /* CRC good, aircraft table updated                */
    uint32_t bad_chips;    /* malformed: invalid chip pairs or wrong length   */
    uint32_t bad_crc;      /* well formed, CRC bad after repair               */
    uint32_t empty_polls;  /* polls that found nothing                        */
    uint32_t errors;       /* polls that failed                               */
} adsb_modes_stats_t;

/* One non-blocking step of the receive loop: take up to `max_frames` frames
   from the source and hand each to adsb_on_chips (ADSB_CHIPS_FULL) with its
   level. Returns the frames handled, or the source's negative error. Call it
   from the one thread that calls adsb_on_sample; they share decoder state. */
int adsb_modes_pump(const adsb_modes_ops_t *ops, adsb_modes_stats_t *stats,
                    int max_frames);

/* ---- the chip-frame log: what the LR20xx session actually delivered ------

   The pump keeps the last ADSB_CHIP_LOG_N raw frames with their level and
   verdict, and counts the frames, verdicts and where in the frame the first
   invalid chip pair fell. It is for telling a misaligned or bit-reversed
   capture (bad pairs from the first bits) from noise triggers (bad pairs
   scattered, or late in the frame). A fixed ring: nothing is allocated, and
   the pump writes it from the thread that reads the chip while the console
   reads a copy. */

#define ADSB_CHIP_LOG_N         8
/* First-bad-bit buckets of 8 message bits: 0-7, 8-15, ... 104-111. */
#define ADSB_FIRST_BAD_BUCKETS  14

typedef struct {
    uint32_t seq;                              /* 1 for the first frame since reset */
    uint8_t  chips[ADSB_MODES_FRAME_BYTES];    /* as the chip delivered them        */
    uint8_t  len;                              /* bytes delivered                   */
    uint8_t  verdict;                          /* adsb_frame_result_t               */
    int16_t  first_bad_bit;                    /* message bit, or -1 for none       */
    int16_t  rssi_tenths;                      /* ADSB_RSSI_NONE when not measured  */
} adsb_chip_rec_t;

typedef struct {
    uint32_t frames, decoded, bad_chips, bad_crc, errors;
    uint32_t clean;                            /* frames with no invalid pair at all */
    uint32_t first_bad[ADSB_FIRST_BAD_BUCKETS];
    uint32_t n_recs;                           /* valid entries in recs, 0..N        */
    adsb_chip_rec_t recs[ADSB_CHIP_LOG_N];     /* oldest first                       */
    /* Rates. A chip frame is one preamble trigger, so frames/s is the trigger
       rate. The window is the last completed seconds up to ADSB_RATE_WINDOW_S
       and 0 when there is not a whole one yet; the average runs from the
       session start (or the last reset) to now. */
    int      rate_window_s;
    float    trigger_per_s, decoded_per_s;
    float    avg_trigger_per_s, avg_decoded_per_s;
    uint32_t elapsed_s;
} adsb_chip_diag_t;

#define ADSB_RATE_WINDOW_S      10

/* The pump calls these; so can a test. `verdict` is an adsb_frame_result_t. */
void adsb_chip_diag_record(const uint8_t *chips, int nbytes, int rssi_tenths,
                           int verdict, int first_bad_bit);
void adsb_chip_diag_note_error(void);
/* Clears the log and the counters, and starts the clock the rates run from. */
void adsb_chip_diag_reset(void);
/* The session began: the rates' clock starts now, the counters are kept. */
void adsb_chip_diag_mark_start(void);
/* A consistent copy: taken under one short critical section. */
void adsb_chip_diag_snapshot(adsb_chip_diag_t *out);

/* Text for the console. Each returns the length it wanted, like snprintf. */
int adsb_chip_diag_format_rec(const adsb_chip_rec_t *rec, char *out, size_t size);
int adsb_chip_diag_format_counts(const adsb_chip_diag_t *d, char *out, size_t size);
int adsb_chip_diag_format_hist(const adsb_chip_diag_t *d, char *out, size_t size);
int adsb_chip_diag_format_rates(const adsb_chip_diag_t *d, char *out, size_t size);
/* The `adsb chips` report, to stdout. */
void adsb_chip_diag_print(void);

#ifdef __cplusplus
}
#endif

#endif
