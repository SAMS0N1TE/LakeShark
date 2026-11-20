/* The lock page: a clock, the state of the unit, and a PIN pad.

   The router draws it while locked (ls_tui_set_lock_page). With no PIN the
   page unlocks on two taps of one key, and nothing locks by itself: the
   screen wakes straight into the app. With a PIN (4-8 digits, kept as a
   salted hash in NVS through the settings worker) the unit locks when the
   screen goes dark and/or at boot, as chosen in SETTINGS > DISPLAY. Radios
   and decoders keep running under the lock; only input is held. */
#ifndef LS_LOCK_H
#define LS_LOCK_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the PIN state, installs the page, and locks if "at boot" is set. */
void ls_lock_init(void);
/* The panel just went dark (auto-dim's screen off, a power mode). */
void ls_lock_dark(void);

bool ls_lock_has_pin(void);

/* SETTINGS rows. Editing asks for the current PIN first when there is one. */
void ls_lock_pin_show(char *b, size_t n);
void ls_lock_pin_edit(void);
void ls_lock_pin_remove(void);
void ls_lock_when_show(char *b, size_t n);
void ls_lock_when_step(int dir);
void ls_lock_when_next(void);

/* "Screen off": seconds from dim to dark, 0 never (display_ctl). */
void ls_lock_off_show(char *b, size_t n);
void ls_lock_off_step(int dir);
void ls_lock_off_next(void);

#ifdef __cplusplus
}
#endif

#endif
