
#ifndef ACARS_APP_H
#define ACARS_APP_H

#include "acars.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Device-side owner of the ACARS message log.  The decoder in acars.c
   writes into a caller-supplied acars_state_t; this file owns that state
   and exposes it to consumers - the LVGL panel that renders messages, and
   whichever radio worker eventually feeds audio into acars_process().

   Kept separate from acars.c so the decoder itself remains a pure-logic
   module the bench can compile and exercise without any of the shared-
   state or firmware plumbing that only makes sense on the device. */

/* Read-only view of the message log.  Never NULL.  The caller must not
   race the writer - update at LVGL timer cadence, not from an ISR. */
const acars_state_t *acars_app_state(void);

/* Mutable pointer, for the radio worker to hand to acars_create(). */
acars_state_t *acars_app_state_mut(void);

/* Wipe the log.  Used by the UI clear button. */
void acars_app_clear(void);

/* Push a synthetic decoded message into the log.  The registration, flight
   and text fields let a UI verify that decoded messages actually reach the
   panel without waiting on a real transmission - the same pattern
   adsb_inject_fake_aircraft uses for ADS-B.  `reg`, `label` and `text` may
   be NULL; missing fields are left blank. */
void acars_app_inject(const char *reg, const char *label, const char *text);

size_t acars_flight_from_text(const char *text, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
