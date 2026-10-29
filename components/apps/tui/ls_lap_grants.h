/* Capabilities the owner granted to an installed native app, remembered
 * across restarts so RADIO_RX / RADIO_TX are asked once per install (NETWORK
 * is still asked on every launch). An install is the app's id, version and
 * file size: an update or a different file asks again. Kept in
 * /sdcard/apps/lap_grants.txt, under the folder apps cannot write.
 *
 * The table is RAM (PSRAM) and safe to ask from the screen task; the card
 * is read at the boot scan and written by the lap_io worker only. */
#ifndef LS_LAP_GRANTS_H
#define LS_LAP_GRANTS_H
#include "ls_lap.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_LAP_GRANT_CAPS (LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX | LAP_CAP_SERVICE)   /* the ones remembered */
#ifndef LS_LAP_GRANTS_PATH
#define LS_LAP_GRANTS_PATH "/sdcard/apps/lap_grants.txt"
#endif
/* True when every bit of caps & LS_LAP_GRANT_CAPS was granted to this install. */
bool ls_lap_grant_has(const ls_lap_info_t *app, uint8_t caps);
/* Remember a grant (screen task); the worker writes it out. */
void ls_lap_grant_set(const ls_lap_info_t *app, uint8_t caps);
/* Boot scan / worker: read the file once (later calls do nothing). */
void ls_lap_grants_load(const char *path);
/* Worker: write the file if a grant changed. False on a write error. */
bool ls_lap_grants_save(const char *path);
#ifdef __cplusplus
}
#endif
#endif
