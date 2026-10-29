/* The fox hunt in progress: bearings kept from FOX HUNT and COMPASS FIND,
   the crossing they make, and the card copy that survives a restart.

   UI-task calls change RAM and leave the file work for the worker
   (ls_fox_io_step, run with RF SURVEY's on the direction-finding task).
   Each hunt is /sdcard/lakeshark/fox/hunt_<stamp>.csv (ls_fox.h). */

#ifndef LS_FOX_RUN_H
#define LS_FOX_RUN_H

#include <stdbool.h>
#include <stdint.h>

#include "ls_fox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keep a bearing: into the hunt, onto the card, and the fix recomputed. */
void ls_fox_keep(const ls_df_bearing_t *b);
/* Take the newest bearing back. */
bool ls_fox_undo(void);
/* A new hunt: RAM cleared; the next bearing starts a new file. */
void ls_fox_new(void);
/* Load the newest hunt from the card (worker), replacing RAM. */
void ls_fox_reload(void);

void ls_fox_get(ls_fox_hunt_t *out);
/* Bearings kept, without copying the hunt. */
int ls_fox_count(void);
/* The crossing of the bearings kept, recomputed when they change. */
bool ls_fox_fix_now(ls_fox_fix_t *out);
uint32_t ls_fox_serial(void);
/* The card's side of it: file name and the last thing that happened. */
const char *ls_fox_status(void);
const char *ls_fox_file(void);

void ls_fox_io_step(void);
int  ls_fox_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
