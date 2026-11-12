/* How SCAN's live columns follow the sweep: pure, so the bench can drive it.

   Each column jumped to any reading above it and fell back 30% a pass. That
   reads well on a receiver whose noise is flat, and badly on one whose
   readings scatter: the LR2021 at 433 MHz, measured beside an SX1262, read
   -130 to -104 dBm over four quiet passes (+/-6 dB on one bin), and the
   instant attack latched every upward wiggle, so the columns rode 10-16 dB
   over the floor and jumped every pass. A reading counts as a signal - and
   gets the instant attack - only when it clears the noise this pass shows:
   three spreads, and never less than 6 dB, over the floor. Everything below
   that is followed evenly. Detection does not use any of this; it compares
   the raw reading with the gate. */
#ifndef REC_SCAN_LIVE_H
#define REC_SCAN_LIVE_H

#define REC_SCAN_LIVE_FOLLOW  0.30f   /* share of the gap closed a pass */
#define REC_SCAN_LIVE_MIN_DB  6.0f    /* a signal clears the floor by this at least */
#define REC_SCAN_LIVE_SPREADS 3.0f

/* The noise spread of one pass: the interquartile range of the sorted row
   over 1.35, which is one standard deviation for Gaussian scatter and is not
   moved by the few bins a transmitter occupies. */
static inline float rec_scan_spread(const float *sorted, int n)
{
    if (n < 4) return 0.0f;
    const float iqr = sorted[(3 * n) / 4] - sorted[n / 4];
    return iqr > 0.0f ? iqr / 1.35f : 0.0f;
}

/* The level a reading must reach to be taken at once. */
static inline float rec_scan_attack_level(float floor_db, float spread_db)
{
    float over = REC_SCAN_LIVE_SPREADS * spread_db;
    if (over < REC_SCAN_LIVE_MIN_DB) over = REC_SCAN_LIVE_MIN_DB;
    return floor_db + over;
}

/* One pass of one column. */
static inline float rec_scan_follow(float shown, float raw, float attack_level)
{
    if (raw > shown && raw >= attack_level) return raw;
    return shown + (raw - shown) * REC_SCAN_LIVE_FOLLOW;
}

#endif
