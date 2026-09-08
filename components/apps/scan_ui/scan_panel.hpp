#pragma once

/*LS-746*/
/* ONE scanner control surface, built once and embedded by every app that can
   scan. It exists because the controls for a SINGLE engine had been scattered
   across two apps as each was edited: scan_engine's SOURCE toggle lived only
   on the P25 SCAN tab, while the SCAN/SKIP buttons and AUTO SQ lived only on
   the FM VFO tab, and the band range could not be set from the UI at all -
   only from the console `scan band`. The result was that the BAND (12.5 kHz
   handheld-style) scan the FM app is the natural home for could only be
   ENABLED from the P25 app, and could never be AIMED without a serial cable.
   That is not a missing feature, it is one feature cut in half.

   This is deliberately a shared WIDGET rather than a new app: the engine is
   already a singleton and already app-agnostic, so what was missing was one
   place to express it, not another owner. Both apps embed the same object, so
   they cannot drift apart again the way they did.

   The owning app must call refresh() from its own periodic timer - the panel
   deliberately does not create an lv_timer of its own, so it cannot outlive
   the app that built it or double up when two apps are resident. */

#include "lvgl.h"

class ScanPanel {
public:
    /* Build the whole surface into `parent`. Safe to call again on relaunch;
       every pointer is re-seated from the new widget tree. */
    void build(lv_obj_t *parent);

    /* Called from the owner's timer. Cheap - it only writes labels whose text
       actually changed (sdr_text_if_changed). */
    void refresh(void);

    /* The owner is being torn down; drop every widget pointer so a late
       refresh() cannot touch freed objects. LVGL frees the widgets with the
       parent, so this does not delete anything itself. */
    void forget(void);

private:
    void applyPreset(int idx);
    void syncStep(void);
    void nudgeEdge(bool start_edge, int direction);

    static void toggleCb(lv_event_t *e);
    static void skipCb(lv_event_t *e);
    static void srcCb(lv_event_t *e);
    /*LS-736*/
    static void presetPrevCb(lv_event_t *e);
    static void presetNextCb(lv_event_t *e);
    static void stepPrevCb(lv_event_t *e);
    static void stepNextCb(lv_event_t *e);
    static void nudgePrevCb(lv_event_t *e);
    static void nudgeNextCb(lv_event_t *e);
    static void startPrevCb(lv_event_t *e);
    static void startNextCb(lv_event_t *e);
    static void stopPrevCb(lv_event_t *e);
    static void stopNextCb(lv_event_t *e);
    static void hangPrevCb(lv_event_t *e);
    static void hangNextCb(lv_event_t *e);
    static void asqCb(lv_event_t *e);

    lv_obj_t *_status   = nullptr;
    lv_obj_t *_run_lbl  = nullptr;
    lv_obj_t *_src_val  = nullptr;
    lv_obj_t *_band_val = nullptr;
    lv_obj_t *_start_val = nullptr;
    lv_obj_t *_stop_val = nullptr;
    lv_obj_t *_step_val = nullptr;
    lv_obj_t *_sq_val   = nullptr;
    lv_obj_t *_hang_val = nullptr;
    /*LS-736*/
    lv_obj_t *_nudge_val = nullptr;

    int _preset = 0;
    int _step   = 0;   /* index into STEPS_HZ */
    /*LS-736*/
    /* index into NUDGES_HZ. 100 kHz to start: it is the finer of the two
       amounts the four-button cluster this replaced offered, so the first
       press of an edge does what the old panel's first press did. */
    int _nudge  = 3;
};
