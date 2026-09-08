#ifndef LS_CTL_H
#define LS_CTL_H

#ifdef __cplusplus
extern "C" {
#endif

void ls_ctl_register_commands(void);
void ls_ctl_start_repl(void);

/* LS-994  The safe-mode console: help, version, crash and safemode only.
   Everything the normal set offers dereferences a backend that safe mode
   deliberately never started. */
void ls_ctl_start_recovery_repl(void);
/* The same reduced set, for a boot path that owns its own REPL object
   (headless_main.c builds one with esp_console_new_repl_uart). */
void ls_ctl_register_recovery_commands(void);

#ifdef __cplusplus
}
#endif

#endif
