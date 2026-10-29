/* DMR on the symbol stream the P25 receiver is already producing.
 *
 * DMR and P25 Phase 1 C4FM are both 4FSK at 4800 symbols per second, so a
 * receiver hunting for a P25 frame sync on a DMR channel has every DMR burst
 * pass through it and throws all of them away. This watches that same stream
 * and reports what it sees. It never steers the radio and never touches the
 * P25 decoder's state.
 *
 * It can be fed two ways. dmr_watch_symbol() takes symbols already timed
 * and sliced by someone else - the P25 frame-sync loop. dmr_watch_samples()
 * takes the C4FM discriminator output and recovers timing and levels itself
 * (dmr_symsync), then goes back over each burst it frames and re-slices it
 * at the phase and levels its sync says are best. Once samples have arrived
 * symbols are ignored, so the two never interleave in one framer.
 *
 * Polarity is not known in advance, and cannot be read off a sync: voice
 * and data syncs are each other's negation. Both are framed; whichever
 * produces a checked block - an LC, CSBK or data header CRC, or an embedded
 * LC - fixes the polarity, and from then on the other is only listened to
 * for a checked block of its own.
 */
#ifndef LS_DMR_WATCH_H
#define LS_DMR_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "dmr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DMR_CALL_TIMEOUT_US 1500000      /* no burst for this long ends a call */

typedef enum {
    DMR_END_NONE = 0,
    DMR_END_TERMINATOR,                  /* Terminator with LC              */
    DMR_END_TIMEOUT,                     /* the slot went quiet             */
    DMR_END_REPLACED,                    /* a different call took the slot  */
} dmr_end_t;

typedef struct {
    dmr_lc_t lc;
    uint8_t colour_code;
    bool have_lc, active, encrypted;
    int64_t start_us, last_us, end_us;
    bool group;                          /* FLCO 0; else unit to unit       */
    bool late_entry;                     /* identity from embedded LC only  */
    bool corrected;                      /* identity needed RS correction   */
    uint8_t end_reason;                  /* dmr_end_t                       */
    uint32_t voice_bursts;
    dmr_alias_t alias;
    bool have_gps;
    dmr_gps_t gps;
} dmr_call_t;

typedef struct {
    uint32_t         bursts;        /* bursts framed                       */
    uint32_t         slot_type_ok;  /* Slot Type inside its Golay radius   */
    uint32_t         bptc_ok;
    uint32_t         bptc_fail;
    uint32_t         lc_ok;         /* LC whose masked RS parity was exact */
    uint32_t         symbols;

    bool             inverted;      /* polarity of the last framed burst   */
    dmr_sync_class_t last_class;
    uint8_t          last_sync_errors;
    uint8_t          colour_code;

    bool             have_lc;
    dmr_lc_t         lc;            /* last full LC that passed parity     */
    int64_t          lc_us;         /* when that was, esp_timer clock      */

    dmr_tracker_t    tracker;
    dmr_call_t       call[2];
    uint32_t         unknown_slot;

    /* Checked blocks other than an exact full LC. None of these alone says
       a channel is DMR - see dmr_watch_verdict(). Each also fixes the
       polarity. */
    uint32_t         lc_fixed;      /* full LC after one-octet RS fix      */
    uint32_t         emb_lc_ok;     /* embedded LC, Hamming and checksum   */
    uint32_t         emb_lc_fail;   /* four fragments that did not check   */
    uint32_t         csbk_ok;
    uint32_t         data_hdr_ok;
    uint32_t         voice_bursts;  /* B-F found by counting from a sync   */

    int8_t           polarity;      /* -1 unknown, 0 normal, 1 inverted    */
    bool             from_samples;  /* timing recovered here, not by P25   */
    uint32_t         refined;       /* bursts re-sliced at the sync's phase */

    bool             have_csbk;
    dmr_csbk_t       csbk;
    uint8_t          csbk_slot;     /* 0 unknown (mobile, no CACH)         */
    int64_t          csbk_us;

    bool             have_data_hdr;
    dmr_data_header_t data_hdr;
    uint8_t          data_hdr_slot;
    int64_t          data_hdr_us;
} dmr_watch_t;

/* Returns 1/2 for a repeater CACH; mobile sync has no slot identity. */
unsigned dmr_cach_slot(const uint8_t cach[3]);

/* Both framers are reset and every counter cleared. */
void dmr_watch_reset(void);

/* One 4FSK symbol from the demodulator, with the slicer thresholds it was
 * measured against - the same center/umid/lmid the P25 path maintains, so
 * this tracks the receiver's AGC rather than keeping its own. Ignored once
 * dmr_watch_samples() has been fed. */
void dmr_watch_symbol(int symbol, int center, int umid, int lmid);

/* C4FM discriminator output, 48 kHz (10 samples a symbol), after the RRC
 * matched filter - what dsp_process_iq() returns in DEMOD_C4FM. */
void dmr_watch_samples(const int16_t *samples, int count);

/* A copy of the published state. Returns false if nothing has been fed. */
bool dmr_watch_get(dmr_watch_t *out);

/* What the counters add up to. Only link control counts: an exact full LC
 * (24 bits of RS parity under a data-type mask) on its own, or two weaker
 * ones - embedded LCs (Hamming rows, column parity and a 5-bit checksum) or
 * full LCs that needed an RS octet fixed - which noise does not produce
 * twice. CSBK and data header CRCs are 16 bits and are reported, not
 * counted. */
typedef enum {
    DMR_VERDICT_NOTHING = 0,             /* no bursts at all                */
    DMR_VERDICT_NOT_DMR,                 /* syncs, nothing checked          */
    DMR_VERDICT_DMR,
} dmr_verdict_t;

dmr_verdict_t dmr_watch_verdict(const dmr_watch_t *w);

/* The `dmr` report: counters, verdict and both slots' calls, as text. */
size_t dmr_watch_format(const dmr_watch_t *w, int64_t now_us,
                        char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LS_DMR_WATCH_H */
