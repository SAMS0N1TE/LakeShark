/* API 2.5 system broker. App calls/UI work on TUI; only service does disk I/O.
   Each resident owns its copied jobs/scopes; no storage worker retains app pointers. */
#ifndef LS_LAP_SYSTEM_H
#define LS_LAP_SYSTEM_H
#include "ls_lap_api.h"
void ls_lap_system_begin(const char *id, uint32_t generation);
void ls_lap_system_end(void);
void ls_lap_system_ephemeral(bool on);
void ls_lap_system_hide(void);
void ls_lap_system_detach(void); /* screen task: retire IPC/notices before IO frees the context */
void ls_lap_system_frame(void);
bool ls_lap_system_service(void);
void ls_lap_system_wake(void (*wake)(void));
void ls_lap_system_input(unsigned kind, int w, int h);
bool ls_lap_system_picker_active(void);
bool ls_lap_system_activate(int notification, int action);
bool ls_lap_system_script(const char *command);
#endif
