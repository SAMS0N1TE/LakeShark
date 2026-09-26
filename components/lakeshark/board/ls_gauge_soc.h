/* State of charge from the pack voltage, for a gauge that cannot learn.

   The T-Display-P4's BQ27220 runs on its factory defaults (3000 mAh design
   capacity), not a profile for the display's small cell, and it restarts
   whenever the cell is disconnected. Its own estimate read 2 % on a cell
   at 4124 mV. The voltage, corrected for the drop across the cell and its
   protection, is the better number. */

#ifndef LS_GAUGE_SOC_H
#define LS_GAUGE_SOC_H

#include <stdint.h>

/* Cell and protection in series. Estimated, not measured:
   a wrong value shows as the percentage jumping when the load changes. */
#define LS_GAUGE_PACK_MOHM 100

/* Rest voltage from a loaded one. Current is positive into the cell. */
static inline int ls_gauge_rest_mv(int millivolts, int milliamps)
{
    return millivolts - milliamps * LS_GAUGE_PACK_MOHM / 1000;
}

/* A generic NMC Li-ion rest-voltage curve, 0..100 %. */
static inline int ls_gauge_soc_from_rest_mv(int mv)
{
    static const int16_t v[] = {3300, 3500, 3610, 3670, 3710, 3750, 3790, 3850, 3920, 4000, 4100, 4180};
    static const uint8_t p[] = {   0,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100};
    const int n = (int)(sizeof(v) / sizeof(v[0]));
    if (mv <= v[0]) return 0;
    if (mv >= v[n - 1]) return 100;
    int i = 1;
    while (mv > v[i]) i++;
    return p[i - 1] + (p[i] - p[i - 1]) * (mv - v[i - 1]) / (v[i] - v[i - 1]);
}

#endif /* LS_GAUGE_SOC_H */
