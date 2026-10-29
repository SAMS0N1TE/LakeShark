#ifndef P25_LR_WIDE_H
#define P25_LR_WIDE_H
/* P25 on the LR2021's wide I/Q: the shared LR session (lr_wf_session.h)
 * captures the 576k route and down-converts it; this sink turns its calls
 * (<=256 pairs of 125k IQ16, NULL/0 at each stream end) back into the
 * 320-pair blocks P25's own queue and front end take, so a block holds as
 * long as on the narrow path (2.56 ms, 20 ms for the queue) instead of the
 * ~111-pair dribble one wide slot yields.
 *
 * Narrow stays P25's own capture task: it is proven and its hop is the
 * same chip recovery. Wide hops with lr_wf_retune, which skips the modem
 * re-program a narrow hop pays; `p25 lrwide` prints both paths' last and
 * worst hop so the board can settle which follows a trunk faster. */
#include "lr2021_iq_queue.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* OFF (narrow) at boot. FOLLOW takes the shared WIDE I/Q setting (LR FRONT
   END, `lora lrwide`); ON pins wide. */
typedef enum { P25_LR_WIDE_FOLLOW = 0, P25_LR_WIDE_ON, P25_LR_WIDE_OFF } p25_lr_wide_pref_t;
void p25_lr_wide_set_pref(p25_lr_wide_pref_t pref);
p25_lr_wide_pref_t p25_lr_wide_pref(void);
bool p25_lr_wide_pick(p25_lr_wide_pref_t pref, bool shared_wide);
const char *p25_lr_wide_pref_name(p25_lr_wide_pref_t pref);

typedef struct {
    lr2021_iq_queue queue;
    /* The P25 task opens it on a fresh queue; the session's owner fills
       it. busy brackets each fill so close can wait it out. */
    atomic_bool open, busy, broken;
    atomic_uint resets;           /* NULL/0 calls: each stream end      */
    uint32_t generation;
    uint64_t pair;                /* next block's first pair            */
    size_t staged;
    uint8_t stage[LR2021_IQ_BLOCK_BYTES];
} p25_lr_wide_sink_t;

/* lr_wf_consumer_t; ctx is the sink. Never waits. */
void p25_lr_wide_sink_consume(void *ctx, const uint8_t *data, size_t bytes, bool spare);
/* P25 side. Close stops filling and returns once no fill is in progress;
   open starts a new generation on an empty queue. */
void p25_lr_wide_sink_close(p25_lr_wide_sink_t *k);
bool p25_lr_wide_sink_open(p25_lr_wide_sink_t *k, uint32_t generation);
#endif
