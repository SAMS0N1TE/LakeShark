#ifndef DISPLAY_CTL_H
#define DISPLAY_CTL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_ctl_init(void);

void display_ctl_set_user(int pct);
int  display_ctl_get_user(void);

void display_ctl_set_autodim(bool enabled);
bool display_ctl_autodim_enabled(void);

void display_ctl_set_autodim_timeout(int seconds);
int  display_ctl_autodim_timeout(void);

/* The TUI's auto-dim clock. activity() is any touch, key or turn of
   the board; tick() runs from the frame loop and gates itself to five times
   a second. Both do nothing on the LVGL boards, where LVGL keeps the clock.
   reapply() puts the panel back at the policy's level after something else
   wrote to it; dimmed() says whether auto-dim has it down right now. */
void display_ctl_activity(void);
void display_ctl_tick(void);
void display_ctl_reapply(void);
bool display_ctl_dimmed(void);

#ifdef __cplusplus
}
#endif

#endif
