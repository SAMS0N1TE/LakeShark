/* HF (shortwave) reach of the USB receivers: the frequency limits apps tune
   against and which signal path an RTL-SDR takes for a frequency.

   RTL-SDR dongles get below the R820T/R828D's ~24 MHz floor two ways:
   - Generic R820T/R820T2 (e.g. Nooelec NESDR): direct sampling on the
     RTL2832U's Q-branch ADC. The tuner is put in standby and the ADC samples
     the antenna (wired to the Q input pins) at 28.8 MHz; the demodulator's
     DDC (the IF frequency register) is set to the wanted frequency. Usable
     0.5 to 28.8 MHz, and only on dongles with the Q input brought out to the
     antenna (RTL-SDR Blog V3 style, or a modified dongle). RTL2832U register
     use from its public register notes: demod page 1 0xb1 (zero-IF off),
     0x15 (spectrum inversion off), page 0 0x08 (ADC enable) and 0x06 (ADC
     input swap, which picks Q).
   - RTL-SDR Blog V4 (USB strings "RTLSDRBlog" / "Blog V4"): its built-in
     upconverter mixes HF up by the 28.8 MHz reference into the R828D's
     cable-2 input, so the tuner is set to f + 28.8 MHz with that input
     selected (tuner_r82xx.c, r82xx_set_freq). No direct sampling.
   The HackRF tunes from 1 MHz on its own front end. */
#ifndef LS_RADIO_HF_H
#define LS_RADIO_HF_H

#include <stdbool.h>
#include <stdint.h>

/* The lowest frequency any app here asks a receiver for. */
#define LS_HF_MIN_HZ             500000u
/* Where HF ends: below this the FM app's AM mode runs the HF demodulators. */
#define LS_HF_TOP_HZ           30000000u
/* The R820T/R828D PLL floor, and the old lower limit of every RTL app. */
#define LS_RTL_TUNER_MIN_HZ    24000000u
/* The RTL2832U ADC clock: direct sampling cannot reach past it. */
#define LS_RTL_DIRECT_MAX_HZ   28800000u
/* The Blog V4 switches to its upconverter below this (tuner_r82xx.c). */
#define LS_RTL_V4_HF_BELOW_HZ  28000000u
#define LS_RTL_V4_UPCONVERT_HZ 28800000u
/* The HackRF's own floor (hackrf_radio.c ls_hackrf_frequency_ranges). */
#define LS_HACKRF_MIN_HZ        1000000u

typedef enum {
    LS_RTL_PATH_NONE = 0,     /* no dongle */
    LS_RTL_PATH_TUNER,        /* R820T/R828D as usual, 24 MHz up       */
    LS_RTL_PATH_DIRECT_Q,     /* RTL2832U Q-branch direct sampling     */
    LS_RTL_PATH_UPCONVERT,    /* Blog V4 HF upconverter into the R828D */
} ls_rtl_path_t;

/* Which path an RTL takes for `hz`. 24 to 28.8 MHz stays on the tuner where
   it can lock: its LNA beats the bare 8-bit ADC. */
static inline ls_rtl_path_t ls_rtl_path_for(uint64_t hz, bool blog_v4)
{
    if (blog_v4) return hz < LS_RTL_V4_HF_BELOW_HZ ? LS_RTL_PATH_UPCONVERT
                                                   : LS_RTL_PATH_TUNER;
    return hz < LS_RTL_TUNER_MIN_HZ ? LS_RTL_PATH_DIRECT_Q : LS_RTL_PATH_TUNER;
}

static inline const char *ls_rtl_path_name(ls_rtl_path_t path)
{
    switch (path) {
    case LS_RTL_PATH_TUNER:     return "tuner";
    case LS_RTL_PATH_DIRECT_Q:  return "direct-Q";
    case LS_RTL_PATH_UPCONVERT: return "V4-upconv";
    default:                    return "none";
    }
}

#endif
