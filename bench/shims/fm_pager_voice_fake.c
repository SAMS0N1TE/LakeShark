/* Host stand-in for the FM backend call scr_fm.c makes from POCSAG OPTIONS
   (HEAR CHANNEL), for targets that fake the rest of the backend inline. */
#include <stdbool.h>
#include "apps/fm/fm_state.h"
void lakeshark_fm_pager_voice(bool on) { FM.pager_voice = on; }
