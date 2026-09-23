/* DMR on the symbol stream the P25 receiver is already producing.
 *
 * DMR and P25 Phase 1 C4FM are both 4FSK at 4800 symbols per second, so a
 * receiver hunting for a P25 frame sync on a DMR channel has every DMR burst
 * pass through it and throws all of them away. This watches that same stream
 * and reports what it sees. It never steers the radio and never touches the
 * P25 decoder's state.
 *
 * Polarity is not known in advance, so both are framed at once and whichever
 * matches a sync pattern wins - the same ambiguity the SX1262 FSK path has.
 */
#ifndef LS_DMR_WATCH_H
#define LS_DMR_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "dmr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t         bursts;        /* bursts framed, either polarity      */
    uint32_t         slot_type_ok;  /* Slot Type inside its Golay radius   */
    uint32_t         bptc_ok;
    uint32_t         bptc_fail;
    uint32_t         lc_ok;         /* LC that passed its masked RS parity */
    uint32_t         symbols;

    bool             inverted;      /* polarity of the last framed burst   */
    dmr_sync_class_t last_class;
    uint8_t          last_sync_errors;
    uint8_t          colour_code;

    bool             have_lc;
    dmr_lc_t         lc;            /* last LC that passed parity          */
    int64_t          lc_us;         /* when that was, esp_timer clock      */

    dmr_tracker_t    tracker;
} dmr_watch_t;

/* Both framers are reset and every counter cleared. */
void dmr_watch_reset(void);

/* One 4FSK symbol from the demodulator, with the slicer thresholds it was
 * measured against - the same center/umid/lmid the P25 path maintains, so
 * this tracks the receiver's AGC rather than keeping its own. */
void dmr_watch_symbol(int symbol, int center, int umid, int lmid);

/* A copy of the published state. Returns false if nothing has been fed. */
bool dmr_watch_get(dmr_watch_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LS_DMR_WATCH_H */
