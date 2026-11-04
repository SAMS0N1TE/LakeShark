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

/* Whether the keyboard backlight dims with the screen. Saved. */
void display_ctl_set_keyboard_dim(bool enabled);
bool display_ctl_keyboard_dim(void);

/* The TUI's auto-dim clock. activity() is any touch, key or turn of
   the board; tick() runs from the frame loop and gates itself to five times
   a second. Both do nothing on the LVGL boards, where LVGL keeps the clock.
   reapply() puts the panel back at the policy's level after something else
   wrote to it; dimmed() says whether auto-dim has it down right now. */
void display_ctl_activity(void);
void display_ctl_tick(void);
void display_ctl_reapply(void);
bool display_ctl_dimmed(void);

/* The screen dark with the UI still running ('display off', or a remote
   terminal's DISPLAY OFF): a finger, a key or a turn of the board lights it
   again, and so does a brightness change. nudge() is input that came from
   elsewhere (a remote terminal, a console tap): it counts for auto-dim but
   leaves a dark screen dark. Not saved. */
void display_ctl_set_off(bool off);
bool display_ctl_off(void);
void display_ctl_nudge(void);

/* A power mode's limits over the user's settings, which stay as they were
   saved: the panel no brighter than cap_pct, dimmed after dim_s idle seconds
   at most (0 leaves auto-dim to the user), and turned off after off_s idle
   seconds (0 never). The keyboard light goes down with the screen whenever
   a dim time is set. FULL is (100, 0, 0). Not saved. */
void display_ctl_set_power_policy(int cap_pct, int dim_s, int off_s);

#ifdef __cplusplus
}
#endif

#endif
