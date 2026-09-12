/* Recording a GPS track onto the card. */

#ifndef LS_TRACK_LOG_H
#define LS_TRACK_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start recording. Safe to call twice; returns ESP_OK if it is
   already running. ESP_ERR_NOT_FOUND when there is no card to write to,
   which is a real answer and not a fault - a board with no card can still do
   everything else. */
esp_err_t ls_track_rec_start(void);

/* Stop, leaving what has been recorded on the card. */
void ls_track_rec_stop(void);

bool ls_track_rec_running(void);

/* Points currently held. */
int  ls_track_points(void);

/* OPEN A TRACK THAT IS ALREADY ON THE CARD, without recording. */

int  ls_track_attach(void);

/* Throw the track away. Refused while recording, because "clear" during a
   walk is almost always a mis-tap and the recording is the thing that cannot
   be got back. */
bool ls_track_clear(void);

/* Write the track out as GPX and return how many points went.

   `path` may be NULL, in which case it picks the next unused
   /sdcard/lakeshark/track-NN.gpx - an export that silently overwrote the
   last one would lose a walk to a second tap. Negative on failure. */
/* Returns the number of points written, 0 when the log is there and
   empty, LS_TRACK_EXPORT_NO_LOG when there is no track on the card at all,
   and LS_TRACK_EXPORT_NO_WRITE when the file could not be written.

   The two failures were one -1, and the one caller said "could not write it"
   for both - so a board that had simply never opened the log blamed the
   card. They are told apart now because they send somebody to two different
   places. */
#define LS_TRACK_EXPORT_NO_LOG   (-1)
#define LS_TRACK_EXPORT_NO_WRITE (-2)
int  ls_track_export(const char *path, char *out_path, size_t out_cap);

typedef void (*ls_track_wpt_fn)(void *file);
void ls_track_set_waypoint_source(ls_track_wpt_fn fn);

/* The thresholds, so the console can move them without a rebuild. Metres of
   movement and seconds of stillness; either at zero turns that rule off. */
void ls_track_set_rule(float min_move_m, uint32_t max_gap_s);
void ls_track_get_rule(float *min_move_m, uint32_t *max_gap_s);

#ifdef __cplusplus
}
#endif

#endif /* LS_TRACK_LOG_H */
