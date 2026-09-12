/* The auto-dim decision and its fade, as pure functions so the host
   bench can hold them to what the settings promise. display_ctl.c runs them
   from whichever clock the board has: LVGL's inactivity timer on the LVGL
   boards, the TUI's own touch-and-key clock on the T-Display-P4. */
#ifndef DISPLAY_DIM_H
#define DISPLAY_DIM_H
#include <stdbool.h>
#include <stdint.h>

/* Dim once the screen has been idle for MORE than the whole timeout, and
   never when auto-dim is off. A non-positive timeout means "never", not
   "at once" - a zero that dimmed the panel the moment it was set would read
   as a broken screen. */
static inline bool display_dim_due(bool enabled, int timeout_s, uint32_t idle_ms)
{
    return enabled && timeout_s > 0 && idle_ms > (uint32_t)timeout_s * 1000u;
}

/* One step of the fade from `applied` toward `target`, never past it. An
   unknown current level (negative) goes straight to the target: there is
   nothing to fade from. */
static inline int display_dim_step(int applied, int target, int step)
{
    if (applied < 0 || step <= 0) return target;
    if (applied < target) return applied + step > target ? target : applied + step;
    if (applied > target) return applied - step < target ? target : applied - step;
    return applied;
}

#endif
