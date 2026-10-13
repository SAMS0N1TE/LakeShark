/* WHAT A VIEW CHANGE MAY DO TO A RUNNING SCAN.

   Turning the page is not retuning. The FM screen used to call
   scan_engine_stop() on the way into the VFO, the pager and the spectrum, so
   looking at the waterfall to see what the scanner had just stopped on ended
   the scan, lost the hold and threw away the position in the list - and
   coming back to SCANNER showed a stopped scanner. That is the one thing a
   scanner must not do.

   The rule, in one place so both the screen and its tests read the same one:

     A VIEW NEVER STOPS THE STORED-CHANNEL SCAN.
     AN OPERATOR ACTION THAT MOVES THE RECEIVER ALWAYS DOES.

   The spectrum is a view of whatever the receiver is doing, so entering it
   from a running scan shows the channel the scan is sitting on and the scan
   keeps hopping. Typing a frequency, swapping A/B, changing demodulator or
   starting the frequency sweep are not views: after any of them the stored
   channel the scanner was on no longer describes where the receiver is, so
   the scan ends.

   THE FREQUENCY SWEEP IS NOT THIS SCAN. FM_MODE_SCAN is the band sweep that
   measures a span and plots it (SPECTRUM / RUN SWEEP); scan_engine is the
   stored-channel scanner that stops and lets you listen. They own the tuner
   on the same terms and are never the same thing, so they are asked about
   separately here. */

#ifndef LS_SCAN_VIEW_H
#define LS_SCAN_VIEW_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The FM screen's pages, in its own order: see PAGES[] in scr_fm.c. */
typedef enum {
    LS_SCAN_VIEW_SCANNER = 0,
    LS_SCAN_VIEW_VFO,
    LS_SCAN_VIEW_PAGER,
    LS_SCAN_VIEW_SPECTRUM,
} ls_scan_view_t;

/* The things an operator does that are not views. */
typedef enum {
    LS_SCAN_OP_TUNE = 0,   /* typed a frequency, stepped, swapped A/B      */
    LS_SCAN_OP_MODE,       /* chose another demodulator                    */
    LS_SCAN_OP_SWEEP,      /* started the frequency sweep (FM_MODE_SCAN)   */
    LS_SCAN_OP_SCAN,       /* started the stored-channel scanner itself    */
} ls_scan_op_t;

/* What is running when the question is asked. */
typedef struct {
    bool scanning;   /* scan_engine_active(): the stored-channel scanner   */
    bool sweeping;   /* FM.mode == FM_MODE_SCAN: the frequency sweep       */
} ls_scan_state_t;

typedef enum {
    LS_SCAN_KEEP = 0,   /* leave the scanner alone; it keeps its position  */
    LS_SCAN_STOP,       /* end the stored-channel scan                     */
} ls_scan_act_t;

/* Always LS_SCAN_KEEP. There is no page you can open that invalidates where
   the receiver is, and this says so in a form a call site can hand to
   ls_scan_view_apply() and a test can assert over every pair. */
ls_scan_act_t ls_scan_view_change(ls_scan_view_t to, ls_scan_state_t st);

/* LS_SCAN_STOP for TUNE, MODE and SWEEP while a scan is running; KEEP for
   SCAN, and KEEP for anything when no scan is running - stopping a stopped
   scanner is not an error but it is not this module's business either. */
ls_scan_act_t ls_scan_view_operate(ls_scan_op_t op, ls_scan_state_t st);

/* Does what the two above decided. The only call that touches the engine. */
void ls_scan_view_apply(ls_scan_act_t act);

/* True when arriving at `to` must leave the frequency sweep behind: the VFO
   is a tuned-carrier view and a sweep is not a carrier. The caller owns the
   sweep itself (ls_wf_fm_sweep), because the waterfall does. */
bool ls_scan_view_ends_sweep(ls_scan_view_t to, ls_scan_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* LS_SCAN_VIEW_H */
