#include "fm_mode_handoff.h"

static bool mode_valid(int mode)
{
    return mode >= 0 && mode < FM_MODE_COUNT && mode != 6;
}

void fm_mode_handoff_request(fm_mode_handoff_t *handoff, fm_mode_t mode)
{
    if (!handoff || !mode_valid((int)mode)) return;
    __atomic_store_n(&handoff->pending_mode, (int)mode, __ATOMIC_RELEASE);
}

bool fm_mode_handoff_take(fm_mode_handoff_t *handoff, fm_mode_t *mode)
{
    if (!handoff) return false;
    int pending = __atomic_exchange_n(&handoff->pending_mode, -1,
                                      __ATOMIC_ACQ_REL);
    if (!mode_valid(pending)) return false;
    if (mode) *mode = (fm_mode_t)pending;
    return true;
}

fm_mode_t fm_mode_handoff_resolve_entry(fm_mode_handoff_t *handoff,
                                        int retained_mode,
                                        fm_mode_t fallback_mode)
{
    fm_mode_t selected = mode_valid(retained_mode)
                       ? (fm_mode_t)retained_mode : fallback_mode;
    if (!mode_valid((int)selected)) selected = FM_MODE_POCSAG;

    fm_mode_t requested;
    if (fm_mode_handoff_take(handoff, &requested)) selected = requested;
    return selected;
}
