/* FRONT END: the SDR's analog end in OPTIONS, the same rows in every app on
   that radio (radio_frontend.h holds the settings).

   An app's OPTIONS table carries LS_RFFE_MENUS: two MENU rows, one per SDR,
   of which only the one for the radio in use shows, titled
   "<APP> > FRONT END / RTL-SDR". Inside: BIAS-T, ON AT PLUG-IN, PPM, and the
   radio's own (RTL: DIGITAL AGC, OFFSET TUNING, DIRECT SAMPLING; HackRF: RF
   AMP, LNA, VGA). A row the radio cannot use now stays and says why.

   Switching the bias-T on or forcing the amp leaves a warning that this
   module turns into a banner. Changes go to the front-end worker, never
   USB from the UI task. */
#ifndef LS_RFFE_UI_H
#define LS_RFFE_UI_H

#include "ls_options.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Weak, so a build or bench without this module simply has no FRONT END
   row: a MENU row whose list is NULL counts as empty and is left out. */
#ifdef LS_RFFE_UI_DEFINES
#define LS_RFFE_WEAK
#else
#define LS_RFFE_WEAK __attribute__((weak))
#endif
extern const ls_opt_ctx_t ls_rffe_ctx_rtl LS_RFFE_WEAK;
extern const ls_opt_ctx_t ls_rffe_ctx_hackrf LS_RFFE_WEAK;
/* The LR2021's: WIDE I/Q (saved) and the waterfall's span view. */
extern const ls_opt_ctx_t ls_rffe_ctx_lr LS_RFFE_WEAK;

#define LS_RFFE_MENUS \
    { .label = "FRONT END", .kind = LS_OPT_MENU, .radios = LS_OPT_RADIO(LS_RSEL_SDR_RTL), \
      .sub = &ls_rffe_ctx_rtl }, \
    { .label = "FRONT END", .kind = LS_OPT_MENU, .radios = LS_OPT_RADIO(LS_RSEL_SDR_HACKRF), \
      .sub = &ls_rffe_ctx_hackrf }, \
    { .label = "FRONT END", .kind = LS_OPT_MENU, .radios = LS_OPT_LORA, \
      .sub = &ls_rffe_ctx_lr }

/* Once at UI start: installs the banner for front-end warnings. */
void ls_rffe_ui_init(void);

#ifdef __cplusplus
}
#endif

#endif
