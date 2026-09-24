#include "p25_sync_confirm.h"

#include <string.h>

void p25_sync_confirm_reset(p25_sync_confirm_t *c)
{
    memset(c, 0, sizeof(*c));
}

bool p25_sync_confirm_frame(p25_sync_confirm_t *c, uint16_t nac,
                            uint32_t now_ms, uint32_t generation)
{
    if (generation != c->generation) {
        const uint32_t confirmed = c->confirmed;
        const uint32_t unconfirmed = c->unconfirmed;
        const uint16_t unconfirmed_nac = c->unconfirmed_nac;
        p25_sync_confirm_reset(c);
        c->generation = generation;
        c->confirmed = confirmed;
        c->unconfirmed = unconfirmed;
        c->unconfirmed_nac = unconfirmed_nac;
    }

    bool ok = c->have_nac && nac == c->confirmed_nac;
    if (!ok && c->have_last && nac == c->last_nac &&
        (uint32_t)(now_ms - c->last_ms) <= P25_SYNC_PAIR_MS)
        ok = true;

    c->have_last = true;
    c->last_ms = now_ms;
    c->last_nac = nac;

    if (ok) {
        c->have_nac = true;
        c->confirmed_nac = nac;
        c->confirmed++;
    } else {
        c->unconfirmed++;
        c->unconfirmed_nac = nac;
    }
    return ok;
}
