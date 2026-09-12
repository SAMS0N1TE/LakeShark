#include "display_ctl.h"
#include "display_dim.h"
#include "settings.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ls_board.h"
#include "ls_panel.h"
#include "esp_timer.h"

#include <math.h>

#define DIM_APPLIED_PCT  10
#define RAMP_STEP_PCT    4
#define TICK_MS          200

static int  s_user_pct    = 80;
static bool s_autodim     = true;
static int  s_timeout_s   = 120;
static bool s_dimmed      = false;
static int  s_applied_pct = -1;

static int perceptual(int slider)
{
    if (slider <= 0)   return 0;
    if (slider >= 100) return 100;
    float y = sqrtf((float)slider / 100.0f);
    int pct = (int)(y * 100.0f + 0.5f);
    if (pct < 5) pct = 5;
    return pct;
}

static void apply_now(int applied)
{
    if (applied == s_applied_pct) return;
    s_applied_pct = applied;
#if LS_HAS_COMPACT_UI
    ls_panel_set_brightness(applied);
#else
    bsp_display_brightness_set(applied);
#endif
}

/* One tick of the policy, whichever clock drives it: dim once the
   screen has been idle past the timeout, fade toward the target a step at a
   time, and snap straight back the moment it is not idle any more. */
static void dim_policy(uint32_t idle_ms)
{
    if (display_dim_due(s_autodim, s_timeout_s, idle_ms)) {
        s_dimmed = true;
    } else if (s_dimmed) {
        s_dimmed = false;
        apply_now(perceptual(s_user_pct));
        return;
    }
    const int target = s_dimmed ? DIM_APPLIED_PCT : perceptual(s_user_pct);
    if (s_applied_pct == target) return;
    apply_now(display_dim_step(s_applied_pct, target, RAMP_STEP_PCT));
}

#if LS_HAS_COMPACT_UI
/* AUTO-DIM ON THE T-DISPLAY-P4, OFF UNTIL IT IS ASKED FOR. */

static int64_t s_last_input_us;
static int64_t s_next_tick_us;

void display_ctl_activity(void)
{
    s_last_input_us = esp_timer_get_time();
    if (s_dimmed) {
        s_dimmed = false;
        apply_now(perceptual(s_user_pct));
    }
}

void display_ctl_tick(void)
{
    const int64_t now = esp_timer_get_time();
    if (now < s_next_tick_us) return;
    s_next_tick_us = now + (int64_t)TICK_MS * 1000;
    const int64_t idle_us = now - s_last_input_us;
    dim_policy(idle_us > 0 ? (uint32_t)(idle_us / 1000) : 0u);
}
#else
/* The LVGL boards: LVGL keeps the inactivity clock and its timer runs the
   policy, exactly as before. */
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    dim_policy(lv_disp_get_inactive_time(NULL));
}

void display_ctl_activity(void) {}
void display_ctl_tick(void) {}
#endif

void display_ctl_init(void)
{
    s_user_pct  = settings_get_brightness();
    s_autodim   = settings_get_autodim();
    s_timeout_s = settings_get_autodim_timeout();
    s_dimmed    = false;
    apply_now(perceptual(s_user_pct));
#if LS_HAS_COMPACT_UI
    s_last_input_us = esp_timer_get_time();
#else
    bsp_display_lock(0);
    lv_timer_create(tick_cb, TICK_MS, NULL);
    bsp_display_unlock();
#endif
}

void display_ctl_reapply(void)
{
    s_applied_pct = -1;
    apply_now(s_dimmed ? DIM_APPLIED_PCT : perceptual(s_user_pct));
}

bool display_ctl_dimmed(void) { return s_dimmed; }

void display_ctl_set_user(int pct)
{
    if (pct < 5)   pct = 5;
    if (pct > 100) pct = 100;
    s_user_pct = pct;
    s_dimmed   = false;
    settings_set_brightness(pct);
    apply_now(perceptual(pct));
}

int display_ctl_get_user(void) { return s_user_pct; }

void display_ctl_set_autodim(bool enabled)
{
    s_autodim = enabled;
    settings_set_autodim(enabled);
#if LS_HAS_COMPACT_UI
    /* Turning it on starts the countdown now, not from whenever the glass
       was last touched - or it would dim the instant it was enabled. */
    s_last_input_us = esp_timer_get_time();
#endif
    if (!enabled && s_dimmed) {
        s_dimmed = false;
        apply_now(perceptual(s_user_pct));
    }
}

bool display_ctl_autodim_enabled(void) { return s_autodim; }

void display_ctl_set_autodim_timeout(int seconds)
{
    if (seconds < 5)   seconds = 5;
    if (seconds > 240) seconds = 240;
    s_timeout_s = seconds;
    settings_set_autodim_timeout(seconds);
#if LS_HAS_COMPACT_UI
    s_last_input_us = esp_timer_get_time();
#endif
}

int display_ctl_autodim_timeout(void) { return s_timeout_s; }
