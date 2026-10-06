#ifndef LS_SEARCH_H
#define LS_SEARCH_H

#include <stdbool.h>
#include <stdint.h>

#include "ls_search_core.h"
#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Repeating wideband search on the RTL: hop the whole range at 2.4 MS/s, pass
   after pass, and keep a table of what stands out. The decisions are in
   ls_search_core.c; this is the radio loop, the console report and the SD
   log. Driven by the `search` console command. */

#define LS_SEARCH_DEFAULT_LO_HZ  136000000ull
#define LS_SEARCH_DEFAULT_HI_HZ  174000000ull

/* Fixed manual gain. A floor that is learned per bin cannot follow an AGC
   that moves under it. 29.7 dB is what the DF source runs the RTL at; the
   `sweep` console command defaults to AGC, so this is not shared with it. */
#define LS_SEARCH_GAIN_TENTHS    297

/* Start a search of [lo_hz, hi_hz). Returns LS_RADIO_OK once the worker has
   the job. LS_RADIO_ERR_BUSY when an app owns the receiver, _UNAVAILABLE with
   no receiver, _INVALID for a range that cannot be planned, _EXISTS when a
   search is already running. */
ls_radio_err_t ls_search_run_start(uint64_t lo_hz, uint64_t hi_hz);

/* Stop and wait for the receiver to be handed back. Keeps the hit table and
   the per-bin state, so `search` and `search dump` still answer. */
void ls_search_run_stop(void);

/* Forget the floors and the hits. Safe while running: it relearns. */
void ls_search_run_clear(void);

bool ls_search_run_active(void);

/* Status line and the top hits, to stdout. */
void ls_search_run_report(void);

/* Every bin's floor, last look and recent history, one line per tune's worth
   of bins, for analysis off the board. Works while running. */
void ls_search_run_dump(void);

/* The `search` console command, an esp_console handler. Registered by
   ls_ctl.c, which both the headless and the GUI main reach. `search start`
   answers with one line, "SEARCH started <lo>-<hi> MHz" or
   "SEARCH refused: <reason>". */
int ls_search_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
