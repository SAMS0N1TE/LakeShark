#ifndef ADSB_DEMO_H
#define ADSB_DEMO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Synthetic aircraft, for testing everything downstream of the decoder. */

/* Create `n` demo aircraft (0 turns them off and clears them). */
void adsb_demo_set(int n);

/* How many are running, 0 when off. */
int  adsb_demo_count(void);

/* Advance them. Called once a second from the ADS-B housekeeping task; does
   nothing when the demo is off. */
void adsb_demo_tick(void);

/* Centre the demo traffic somewhere. Defaults to a fixed point so a bench with
   no GPS still produces a plausible map. */
void adsb_demo_center(float lat, float lon);

#ifdef __cplusplus
}
#endif

#endif
