/* "With you": the one rule every app uses to say a device is moving with
   the board (a tag in the bag, a car's radio two lanes over), and its one
   setting.

   The rule: heard with the board for at least `minutes`, while the board
   moved at least `metres` (or was heard at LS_TAIL_PLACES separate places).
   Without a position only the time counts, and the apps say so.

   SWEEP's WITH YOU section and alarm and WARDRIVE's FOLLOW tag both ask
   here. Each adds its own evidence on top: SWEEP wants a separated tracker;
   WARDRIVE wants four places and four odometer steps and never calls a
   device a follower before the board has gone past that kind's radio range
   (ls_tail_need_m), then groups followers into vehicles.

   The setting is saved in NVS ("tail"/"rule_v1") and set with the console
   verb `sweep withyou <minutes> <metres>`. Readers never block. */
#ifndef LS_TAIL_H
#define LS_TAIL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_TAIL_MINUTES_DEF 10
#define LS_TAIL_METRES_DEF  500
#define LS_TAIL_MINUTES_MIN 1
#define LS_TAIL_MINUTES_MAX 240
#define LS_TAIL_METRES_MIN  50
#define LS_TAIL_METRES_MAX  20000
#define LS_TAIL_PLACES      3   /* separate places that count as moved */

typedef struct {
    uint16_t minutes;
    uint16_t metres;
} ls_tail_rule_t;

void ls_tail_get(ls_tail_rule_t *out);
/* Range-checked, applied at once and saved; false when out of range (nothing
   changes) or when the save failed (applied anyway, until the next boot). */
bool ls_tail_set(const ls_tail_rule_t *in);
/* Back to the defaults, not saved (tests, a factory reset path). */
void ls_tail_defaults(void);
/* Read the saved setting once; the defaults hold until then. Call from a task
   that may touch NVS (it goes through ls_nvs_run on the board). */
void ls_tail_load(void);

uint32_t ls_tail_ms(void);              /* the minutes, in milliseconds */
float ls_tail_metres(void);
/* Heard together long enough. */
bool ls_tail_time_ok(uint32_t together_ms);
/* The board moved enough while hearing it. */
bool ls_tail_moved_ok(float moved_m, unsigned places);
/* The whole rule; `gps` false: time only. */
bool ls_tail_with_you(uint32_t together_ms, float moved_m, unsigned places, bool gps);
/* The distance a device of a given radio range must be heard over: the
   setting, but never less than the range itself (a Wi-Fi access point is
   heard over 1.5 km of road without moving). */
float ls_tail_need_m(float range_m);

#ifdef __cplusplus
}
#endif
#endif
