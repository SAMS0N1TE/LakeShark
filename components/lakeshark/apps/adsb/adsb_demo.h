#ifndef ADSB_DEMO_H
#define ADSB_DEMO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*LS-835  Synthetic aircraft, for testing everything downstream of the decoder.

   Positions only exist when a real aircraft is overhead AND its CPR pair has
   been matched, which makes the whole chain below the decoder - the telemetry
   frame, the BLE link, the head's parser, the map - untestable on a bench at
   the wrong time of day. That is a lot of code whose first real exercise would
   otherwise be in a field.

   These are injected through adsb_state_find_or_create(), the same call the
   decoder uses, so nothing downstream can tell the difference or needs to
   know. They are deliberately obvious in the places a human looks - callsigns
   are DEMOnn and the ICAOs are in the 0xF00000 range, which is not assigned to
   real aircraft - so a screen full of them cannot be mistaken for traffic.

   Off by default and not persisted. */

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
