/* See ls_motion.h. Two ideas, both pure functions of the clock. */
#include "ls_motion.h"

#include "esp_timer.h"

int ls_motion_phase(int steps, int period_ms)
{
    if (steps <= 1 || period_ms <= 0) return 0;
    const int64_t ms = esp_timer_get_time() / 1000;
    const int p = (int)((ms % period_ms) * steps / period_ms);
    return (p < 0) ? 0 : (p >= steps ? steps - 1 : p);
}

static const char PIP[4] = {
    LS_TUI_QUAD(1, 0, 0, 0),
    LS_TUI_QUAD(0, 1, 0, 0),
    LS_TUI_QUAD(0, 0, 0, 1),
    LS_TUI_QUAD(0, 0, 1, 0),
};

char ls_motion_pip(bool live)
{

    if (!live) return '.';
    return PIP[ls_motion_phase(4, 800)];
}

void ls_motion_busy(tui_surface *sf, tui_rect panel, bool active)
{
    if (!sf || !active || panel.w < 36 || panel.h < 1) return;
    int phase = ls_motion_phase(12, 1200);
    int head = phase < 6 ? phase : 11 - phase;
    char rail[] = "[......]";
    rail[head + 1] = '>';
    if (phase >= 6) rail[head + 1] = '<';
    tui_put_str(sf, panel, panel.x + panel.w - 10, panel.y, rail,
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
}

uint8_t ls_fresh(ls_fresh_t *f, uint32_t token, int fade_ms)
{
    if (!f) return 0;
    if (!f->seeded) {

        f->seeded = true;
        f->token = token;
        f->since_us = 0;
        return 0;
    }
    if (token != f->token) {
        f->token = token;
        f->since_us = esp_timer_get_time();
    }
    return ls_fresh_level(f, fade_ms);
}

void ls_fresh_bump(ls_fresh_t *f)
{
    if (!f) return;
    f->seeded = true;
    f->since_us = esp_timer_get_time();
}

uint8_t ls_fresh_level(const ls_fresh_t *f, int fade_ms)
{
    if (!f || !f->since_us || fade_ms <= 0) return 0;
    const int64_t age = (esp_timer_get_time() - f->since_us) / 1000;
    if (age < 0) return 0;
    if (age >= fade_ms) return 0;
    return (uint8_t)(255 - (age * 255 / fade_ms));
}

uint8_t ls_fresh_attr(uint8_t level, uint8_t hot_fg, uint8_t cool_fg,
                      uint8_t bg)
{
    /* One step, not a ramp. See the header. Half way is where a fade stops
       being obvious and starts being a colour somebody has to interpret. */
    return TUI_ATTR(level >= 128 ? hot_fg : cool_fg, bg);
}
