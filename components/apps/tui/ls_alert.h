/* One alert-mode control for every detector (SWEEP, WARDRIVE HUNT, PARTY,
   and the rest as they move over): the same five modes, the same words, the
   same order, everywhere a user picks how a find should get their attention.

     OFF          logged, nothing said
     BANNER       a quiet banner
     BUZZ         the motor only: no light, no sound (for a pocket)
     SOUND        banner, sound and the motor
     BANNER+BUZZ  banner and the motor, no sound

   The values are what is saved (PARTY's profiles already hold 0..3 this way),
   so the order on screen is the value order. Header only: modules use it
   without a new firmware export. */
#ifndef LS_ALERT_H
#define LS_ALERT_H

#include <stdbool.h>

enum { LS_ALERT_OFF = 0, LS_ALERT_BANNER = 1, LS_ALERT_BUZZ = 2, LS_ALERT_SOUND = 3, LS_ALERT_BOTH = 4, LS_ALERT_N = 5 };

/* The words, by value, for an ls_opt cycle row (.names = LS_ALERT_NAMES, .n = LS_ALERT_N). */
static const char *const LS_ALERT_NAMES[LS_ALERT_N] = { "OFF", "BANNER", "BUZZ", "SOUND", "BANNER+BUZZ" };

static inline const char *ls_alert_name(int m) { return m >= 0 && m < LS_ALERT_N ? LS_ALERT_NAMES[m] : "?"; }
static inline bool ls_alert_banner(int m) { return m == LS_ALERT_BANNER || m == LS_ALERT_SOUND || m == LS_ALERT_BOTH; }
static inline bool ls_alert_buzz(int m)   { return m == LS_ALERT_BUZZ || m == LS_ALERT_SOUND || m == LS_ALERT_BOTH; }
static inline bool ls_alert_sound(int m)  { return m == LS_ALERT_SOUND; }

#endif
