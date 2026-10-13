/* SCANNER OPTIONS: everything the scanner can be told, in the one list.

   The scanner's panel carries six buttons, and six buttons cannot hold hang
   time, the squelch threshold, the priority dwell, the zone, the band edges,
   the step, the autosquelch and the three per-channel flags. Before this they
   were reachable only from the serial console (ls_ctl) or not at all, which
   is a strange place to keep the settings of a scanner on a handheld.

   So they are rows in the house OPTIONS list (ls_options.h), which already
   owns keypad entry, range refusal and the < > stepping. The table is const
   and the per-channel rows act on the row the panel has selected, handed in
   when the list is opened.

   WHAT STOPS A RUNNING SCAN AND WHAT DOES NOT. Hang, threshold, priority and
   the autosquelch describe HOW the scan runs and are applied live. The
   source, the zone, the band edges, the step and any edit to a channel
   describe WHAT is scanned - after one of those the position in the list no
   longer means anything, so those stop the scan first, which is the rule the
   panel's own ZONE and ENABLE buttons already followed. */

#ifndef LS_SCAN_OPTIONS_H
#define LS_SCAN_OPTIONS_H

#include <stdbool.h>

#include "ls_options.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The list for this scanner. `fm` picks the NFM-only rows (the autosquelch
   measures post-demod noise, which is not a P25 concept); `channel` is the
   scan_channels index the per-channel rows act on, or -1 when the panel has
   nothing selected, which leaves those rows in the list saying so. */
const ls_opt_ctx_t *ls_scan_options(bool fm, int channel);

/* The channel index the last ls_scan_options() was given. */
int ls_scan_options_channel(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_SCAN_OPTIONS_H */
