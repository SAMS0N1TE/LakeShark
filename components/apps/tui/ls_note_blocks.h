/* The lines NOTES inserts at the cursor: live readings written as
   "> TAG ..." so they read as plain text on a computer and as data here.
   The format functions are pure and take what they print; the live_*
   wrappers gather it from the running firmware. */

#ifndef LS_NOTE_BLOCKS_H
#define LS_NOTE_BLOCKS_H

#include <stdbool.h>
#include <stddef.h>
#include "ls_gps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every function writes one or more complete lines ending in '\n', or an
   empty string when there is nothing true to say, and returns the length. */
size_t ls_note_fmt_time(char *out, size_t cap, const char *stamp);
size_t ls_note_fmt_gps(char *out, size_t cap, const ls_gps_state_t *g, bool fresh);
size_t ls_note_fmt_heading(char *out, size_t cap, float magnetic, float true_deg,
                           float tilt_deg, bool calibrated);
size_t ls_note_fmt_map(char *out, size_t cap, double lat, double lon, int zoom,
                       const char *picture);
size_t ls_note_fmt_bearing(char *out, size_t cap, float true_deg, float spread_deg,
                           const char *source, float level, const char *unit,
                           double lat, double lon, bool placed);
size_t ls_note_fmt_sensors(char *out, size_t cap, float ax, float ay, float az,
                           float field_ut, float temp_c, float battery_v);

/* One line per radio group ("p25", "fm", ...) from the named values that
   group publishes right now; "" when the group has nothing live. */
size_t ls_note_live_radio(char *out, size_t cap, const char *group);
/* The groups worth offering, in display order, and a label for each. */
int  ls_note_radio_groups(const char **groups, const char **labels, int cap);
size_t ls_note_live_gps(char *out, size_t cap);
size_t ls_note_live_time(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
