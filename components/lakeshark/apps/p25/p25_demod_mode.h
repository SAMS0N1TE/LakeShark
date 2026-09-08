#ifndef P25_DEMOD_MODE_H
#define P25_DEMOD_MODE_H

#include "dsp_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Task 655: one place that turns an integer into a demod_mode_t. Every
 * source of a mode index - settings NVS, the Flipper link, the console -
 * runs its value through this helper before it reaches dsp_set_mode(). If
 * the enum grows, this is the one thing to update.
 */
demod_mode_t p25_demod_mode_clamp(int idx, demod_mode_t deflt);

#ifdef __cplusplus
}
#endif

#endif
