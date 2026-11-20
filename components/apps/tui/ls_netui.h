/* The hub's screens (core/ls_hub.h does the network): the SEND REPORT sheet that DIAG and SETTINGS
   draw over themselves, the LOCKER and CHAT apps, and the offer to send a report after a crash. */
#ifndef LS_NETUI_H
#define LS_NETUI_H

#include <stdbool.h>
#include "ls_tui_screen.h"
#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The sheet: a host screen opens it, then while ls_report_sheet_active() hands it the whole
   area and its keys and taps (ESC and CLOSE shut it). */
void ls_report_sheet_open(void);
bool ls_report_sheet_active(void);
void ls_report_sheet_draw(tui_surface *sf, tui_rect area);
bool ls_report_sheet_key(ls_tk_t key, char ch);
bool ls_report_sheet_touch(int col, int row);
/* The sheet's keys, for the host screen's hint row while it is up. */
#define LS_REPORT_SHEET_HINT "S send  N note  G place  X close  ESC back"

/* LOCKER and CHAT on HOME (SYSTEM), and the after-crash notice. Once, from the UI setup. */
void ls_netui_register(void);
/* Called once a frame from the notice poll: the crash offer, a sent report's id, CHAT's tile line. */
void ls_netui_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_NETUI_H */
