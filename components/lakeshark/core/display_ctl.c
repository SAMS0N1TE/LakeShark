#include "display_ctl.h"
#include "settings.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include <math.h>

#define DIM_APPLIED_PCT  10
#define RAMP_STEP_PCT    4
#define TICK_MS          200

static int  s_user_pct    = 80;
static bool s_autodim     = true;
static int  s_timeout_s   = 30;
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
    bsp_display_brightness_set(applied);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;

    uint32_t idle = lv_disp_get_inactive_time(NULL);
    if (s_autodim && idle > (uint32_t)s_timeout_s * 1000U) {
        s_dimmed = true;
    } else if (s_dimmed) {
        s_dimmed = false;
        apply_now(perceptual(s_user_pct));
        return;
    }

    int target = s_dimmed ? DIM_APPLIED_PCT : perceptual(s_user_pct);
    if (s_applied_pct == target) return;

    int next = s_applied_pct;
    if (next < target) { next += RAMP_STEP_PCT; if (next > target) next = target; }
    else               { next -= RAMP_STEP_PCT; if (next < target) next = target; }
    apply_now(next);
}

void display_ctl_init(void)
{
    s_user_pct  = settings_get_brightness();
    s_autodim   = settings_get_autodim();
    s_timeout_s = settings_get_autodim_timeout();
    s_dimmed    = false;
    apply_now(perceptual(s_user_pct));

    bsp_display_lock(0);
    lv_timer_create(tick_cb, TICK_MS, NULL);
    bsp_display_unlock();
}

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
}

int display_ctl_autodim_timeout(void) { return s_timeout_s; }
