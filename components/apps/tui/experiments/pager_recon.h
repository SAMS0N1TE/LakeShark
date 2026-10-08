/* PAGER RECON's pure half: the channel plans, the receive settings it tries
   on each channel, and what one received POCSAG batch or FLEX sync says
   about the transmitter, with no radio and no clock in it, so the bench can
   hold it to real codewords.

   WHERE IT LISTENS, AND WHY THERE.
   The scan plans are UHF only: the VHF paging channels (152.24, 152.84,
   157.74, 158.10, 158.70, 163.25 hospital paging) are not in them. That is
   the plans' choice, not the chip's range: a one-frequency run from the
   console accepts 150 MHz and up, and the LR2021 receives VHF POCSAG there.
   What the plans do cover:

     - 929.0125-929.9875 MHz, 40 channels on a 25 kHz raster: Part 90
       private carrier paging. A mix of FLEX networks and POCSAG.
     - 931.0125-931.9875 MHz, 40 channels: Part 22 common carrier paging
       (47 CFR 22.531). Mostly FLEX today, with POCSAG holdouts.
     - 454.025-454.650 MHz, 26 channels: the Part 22 UHF paging and
       radiotelephone base channels (GA-GZ). Legacy POCSAG where used.
     - On-site paging: 457.525-457.600 and 467.750-467.925 MHz, low-power
       Part 90 business channels used by hospital and restaurant pagers.
       Short range, so a list of its own.

   930-931 MHz is narrowband PCS (FLEX/ReFLEX at 3200/6400, 4-level) and
   470-512 MHz is T-band land mobile with no paging allocation, so neither is
   scanned.

   POCSAG has no single rate or polarity: 512, 1200 and 2400 bps are all in
   use, and some transmitters send inverted. The radio matches one 32-bit sync
   word per session, so each (rate, polarity) is its own probe. FLEX is
   probed too, by its 32-bit sync marker at 1600 bps, so that a busy channel
   that will never sync as POCSAG is named rather than left a mystery. */
#ifndef PAGER_RECON_H
#define PAGER_RECON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pocsag.h"

#define PGR_STREAM_RATE 19200u
#define PGR_STREAM_BW   20000u  /* LR2021 snaps upward to 20833 Hz */
#define PGR_STREAM_TRIM_MAX 6000

#ifdef __cplusplus
extern "C" {
#endif

#define PGR_CH_MAX 128

typedef enum { PGR_PLAN_UHF, PGR_PLAN_ONSITE, PGR_PLAN_N } pgr_plan_t;

/* The plan's channel centres in Hz, ascending within each band; returns the
   count, at most `max`. */
int pgr_plan(pgr_plan_t which, uint32_t *hz, int max);
const char *pgr_plan_name(pgr_plan_t which);

/* What one probe asks of the FSK session. */
typedef enum { PGR_POCSAG, PGR_FLEX } pgr_kind_t;
typedef struct {
    uint8_t  kind;          /* pgr_kind_t                                    */
    bool     inverted;      /* the sync word is the complement               */
    uint16_t baud;
    uint32_t deviation_hz;
    uint32_t sync_word;     /* as the radio is given it, 32 bits             */
    uint8_t  payload_bytes; /* what follows the sync word                    */
    uint16_t slice_ms;      /* how long one try lasts before the next probe  */
    char     tag[7];        /* "1200N", "FLEX I"                             */
} pgr_probe_t;

/* POCSAG 1200 N/I, 512 N/I, 2400 N/I, then FLEX N/I: the order is how
   common each is, so a short burst meets the likely one first. */
#define PGR_N_PROBES 8
const pgr_probe_t *pgr_probe(int i);
bool pgr_probe_is_flex(int i);

#define PGR_POCSAG_FSC  0x7CD215D8u
#define PGR_POCSAG_IDLE 0x7A89C197u
#define PGR_FLEX_MARKER 0xA6C6AAAAu
/* 64 bytes of codewords, which is 16 codewords: one POCSAG batch. */
#define PGR_BATCH_BYTES 64

/* One batch, codeword by codeword. Capcodes are the 18 address bits with
   the three frame bits under them, as pagers are programmed. */
typedef struct {
    uint8_t  clean, fixed, bad;     /* BCH: as sent, one bit put right, lost */
    uint8_t  idle, address, message;
    uint8_t  n_caps;
    uint32_t caps[16];
} pgr_batch_t;

/* Reads `data` (PGR_BATCH_BYTES, as the radio delivered it after the sync
   word) into `out`; `inverted` when the sync that framed it was the
   complement. Returns how many codewords were good (clean or fixed). */
int pgr_scan_batch(const uint8_t *data, bool inverted, pgr_batch_t *out);

/* A batch is POCSAG, and not noise that matched the sync, when at least half
   its codewords pass BCH: a random word passes about one time in sixty. */
bool pgr_batch_real(const pgr_batch_t *b);

/* The FLEX mode code that follows the sync marker, from the two payload
   bytes. Returns its index (0..4), or -1 when the bytes are no FLEX code. */
int pgr_flex_mode(const uint8_t *two, bool inverted);
/* "1600/2", "3200/4", ... */
const char *pgr_flex_mode_name(int mode);

/* Distinct capcodes on one channel, for a count. */
#define PGR_CAPS_MAX 48
typedef struct {
    uint16_t n;
    bool     overflow;              /* more than PGR_CAPS_MAX were seen */
    uint32_t v[PGR_CAPS_MAX];
} pgr_capset_t;
/* True when `cap` is new to the set. */
bool pgr_capset_add(pgr_capset_t *s, uint32_t cap);

/* A floor under the bit error rate, in percent: a fixed codeword had one bit
   wrong, one past fixing at least two. -1 when no codeword was read. */
float pgr_ber_pct(uint32_t fixed, uint32_t bad, uint32_t codewords);

/* The median of `n` values, which are reordered. 0 for none. */
float pgr_median(float *v, int n);

/* "12s", "4m", "3h", "2d", or "-" for never (`age_us` < 0). */
void pgr_age(char *out, size_t n, int64_t age_us);

/* Three timing loops share the sign samples, each using the FM decoder. */
typedef struct {
    uint32_t clean, fixed, bad, address, message, frames;
    uint8_t batch_good, n_caps;
    uint32_t caps[16];
} pgr_stream_count_t;

typedef struct {
    pocsag_ctx_t *decoder[3];
    pgr_stream_count_t counts[3];
    pgr_capset_t caps[3];
    uint32_t density_n, density_ones;
    float lowpass[3];
    int trim_hz, want_trim_hz;
    float bias;
} pgr_stream_t;

bool pgr_stream_init(pgr_stream_t *s, fm_state_t *text);
void pgr_stream_free(pgr_stream_t *s);
void pgr_stream_seam(pgr_stream_t *s);
void pgr_stream_feed(pgr_stream_t *s, const uint8_t *data, size_t n);
int pgr_stream_best(const pgr_stream_t *s);

#ifdef __cplusplus
}
#endif

#endif /* PAGER_RECON_H */
