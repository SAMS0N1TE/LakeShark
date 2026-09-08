#include "p25_demod_pref_cache.h"

#define P25_DEMOD_PREF_AUTO       (-1)
#define P25_DEMOD_PREF_MANUAL_MAX 3
#define P25_DEMOD_PREF_STORED_AUTO 4u

/* Defaults to AUTO even if a defensive caller asks before settings_init().
 * An aligned int load/store is atomic on the P4; volatile keeps the cache
 * visible between the boot/settings caller and the receive task. */
static volatile int s_preference = P25_DEMOD_PREF_AUTO;

void p25_demod_pref_cache_init(bool found, uint8_t stored)
{
    if (found && stored <= P25_DEMOD_PREF_MANUAL_MAX)
        s_preference = (int)stored;
    else
        s_preference = P25_DEMOD_PREF_AUTO;
}

int p25_demod_pref_cache_get(void)
{
    return s_preference;
}

bool p25_demod_pref_cache_update(int preference, uint8_t *stored)
{
    if (!stored || preference < P25_DEMOD_PREF_AUTO ||
        preference > P25_DEMOD_PREF_MANUAL_MAX)
        return false;

    s_preference = preference;
    *stored = preference == P25_DEMOD_PREF_AUTO
                  ? P25_DEMOD_PREF_STORED_AUTO
                  : (uint8_t)preference;
    return true;
}
