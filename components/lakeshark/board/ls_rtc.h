/* PCF8563 real-time clock. */

#ifndef LS_RTC_H
#define LS_RTC_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probe the part and note whether it answered. Safe to call more than once. */
esp_err_t ls_rtc_start(void);
bool      ls_rtc_present(void);

/* Read the calendar. ESP_ERR_INVALID_STATE when the chip reports its
   oscillator stopped - the registers hold something, but it is not a time.
   `out` is UTC; the part has no notion of a zone. */
esp_err_t ls_rtc_get(time_t *out);

/* Write the calendar and clear the low-voltage flag. `t` is UTC epoch
   seconds and must be after 2000-01-01, which is the part's own epoch. */
esp_err_t ls_rtc_set(time_t t);

/* Seed the system clock from the RTC at boot. Returns true when a valid time
   was found and settimeofday() was called - which is what makes
   ls_time_is_synced() true without a network. */
bool ls_rtc_seed_system_time(void);

/* Console helper. */
void ls_rtc_diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_RTC_H */
