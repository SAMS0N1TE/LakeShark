#include "dmr.h"

#include <string.h>

void dmr_tracker_reset(dmr_tracker_t *t)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));
}

int dmr_tracker_burst(dmr_tracker_t *t, unsigned int slot, dmr_sync_class_t class_id)
{
    if (!t || slot < 1u || slot > 2u)
        return 0;

    dmr_slot_t *s = &t->slot[slot - 1u];

    switch (class_id) {
    case DMR_SYNC_BS_VOICE:
    case DMR_SYNC_MS_VOICE:
        s->state  = DMR_SLOT_VOICE;
        s->bursts++;
        s->losses = 0;
        break;
    case DMR_SYNC_BS_DATA:
    case DMR_SYNC_MS_DATA:
        s->state  = DMR_SLOT_DATA;
        s->bursts++;
        s->losses = 0;
        break;
    case DMR_SYNC_NONE:
    default:
        /* A slot may drop a single burst without collapsing its state - the
         * other slot's activity is unaffected. The state stays live so a
         * follow-up voice-B burst still lands in the correct superframe. */
        s->losses++;
        s->total_losses++;
        break;
    }
    return 1;
}
