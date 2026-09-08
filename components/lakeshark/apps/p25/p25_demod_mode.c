#include "p25_demod_mode.h"

demod_mode_t p25_demod_mode_clamp(int idx, demod_mode_t deflt)
{
    if (idx >= (int)DEMOD_C4FM && idx <= (int)DEMOD_FSK4_TRACKING)
        return (demod_mode_t)idx;
    if ((int)deflt >= (int)DEMOD_C4FM && (int)deflt <= (int)DEMOD_FSK4_TRACKING)
        return deflt;
    return DEMOD_C4FM;
}
