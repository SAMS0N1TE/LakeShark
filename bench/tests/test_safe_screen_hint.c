

#include "ls_test.h"
#include "ui/ls_safe_screen_hint.h"
#include "ui/ls_ui_logic.h"

#include <string.h>

/* The screen's real configuration - see LS_SAFE_SCREEN_HOLD_MS and the two
   captions main.cpp passes. Not included from ls_safe_screen.h: that header
   pulls in LVGL, which the bench does not build. */
#define HOLD_MS   1500u
#define PRIMARY   "TRY NORMAL BOOT"
#define SECONDARY "STAY SAFE"

/* One press-to-release gesture on one control, driven through the shared
   confirmation exactly as ls_safe_screen.cpp's event handler does. */
static void gesture(ls_safe_hint_t *hint, const char *action,
                    uint32_t press_ms, uint32_t release_ms, uint32_t step_ms)
{
    ls_ui_confirm_state_t confirm;
    ls_ui_confirm_init(&confirm, HOLD_MS);

    ls_ui_confirm_effect_t eff = ls_ui_confirm_step(&confirm,
                                                    LS_UI_CONFIRM_PRESS,
                                                    press_ms);
    ls_safe_hint_update(hint, eff, action,
                        ls_ui_confirm_remaining_ms(&confirm, press_ms));

    for (uint32_t t = press_ms + step_ms; t < release_ms; t += step_ms) {
        eff = ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_UPDATE, t);
        ls_safe_hint_update(hint, eff, action,
                            ls_ui_confirm_remaining_ms(&confirm, t));
    }

    eff = ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_RELEASE, release_ms);
    ls_safe_hint_update(hint, eff, action,
                        ls_ui_confirm_remaining_ms(&confirm, release_ms));
}

static void render(const ls_safe_hint_t *hint, char *out)
{
    size_t n = ls_safe_hint_format(hint, out, LS_SAFE_HINT_TEXT_MAX);
    LS_EQ_UINT((unsigned)n, (unsigned)strlen(out));
}

/* --------------------------------------------------------- before touch -- */

LS_CASE(idle_hint_states_the_hold_and_its_duration)
{
    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, HOLD_MS);
    LS_EQ_INT(hint.phase, LS_SAFE_HINT_READY);

    char text[LS_SAFE_HINT_TEXT_MAX];
    render(&hint, text);

    LS_CHECK_MSG(strstr(text, "1.5 s") != NULL, "idle hint: '%s'", text);
    LS_CHECK_MSG(strstr(text, "HOLD") != NULL, "idle hint: '%s'", text);
    /* And it has to say what a tap will do, because a tap is what was tried. */
    LS_CHECK_MSG(strstr(text, "TAP") != NULL, "idle hint: '%s'", text);
    LS_EQ_INT(ls_safe_hint_color(&hint), LS_UI_COLOR_DIM_TEXT);
}

/* ----------------------------------------------------------- countdown --- */

LS_CASE(countdown_names_the_action_and_the_time_left)
{
    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, HOLD_MS);

    ls_ui_confirm_state_t confirm;
    ls_ui_confirm_init(&confirm, HOLD_MS);

    ls_ui_confirm_effect_t eff = ls_ui_confirm_step(&confirm,
                                                    LS_UI_CONFIRM_PRESS, 1000);
    LS_CHECK(ls_safe_hint_update(&hint, eff, PRIMARY,
                                 ls_ui_confirm_remaining_ms(&confirm, 1000)));
    LS_EQ_INT(hint.phase, LS_SAFE_HINT_COUNTDOWN);

    char text[LS_SAFE_HINT_TEXT_MAX];
    render(&hint, text);
    LS_CHECK_MSG(strstr(text, PRIMARY) != NULL, "armed hint: '%s'", text);
    LS_CHECK_MSG(strstr(text, "1.5 s") != NULL, "armed hint: '%s'", text);

    /* 300 ms in, 1.2 s remain. */
    eff = ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_UPDATE, 1300);
    LS_EQ_INT(eff, LS_UI_CONFIRM_EFFECT_PROGRESS);
    LS_CHECK(ls_safe_hint_update(&hint, eff, PRIMARY,
                                 ls_ui_confirm_remaining_ms(&confirm, 1300)));
    render(&hint, text);
    LS_CHECK_MSG(strstr(text, "1.2 s") != NULL, "progress hint: '%s'", text);
    LS_EQ_INT(ls_safe_hint_color(&hint), LS_UI_COLOR_WARN);

    /* 1 ms before completion the countdown must still ask for a hold, not
       read 0.0 s and look finished. */
    eff = ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_UPDATE, 2499);
    ls_safe_hint_update(&hint, eff, PRIMARY,
                        ls_ui_confirm_remaining_ms(&confirm, 2499));
    render(&hint, text);
    LS_CHECK_MSG(strstr(text, "0.0 s") == NULL, "tail hint: '%s'", text);
    LS_CHECK_MSG(strstr(text, "0.1 s") != NULL, "tail hint: '%s'", text);
}

LS_CASE(countdown_repaints_only_when_the_tenth_changes)
{
    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, HOLD_MS);

    ls_ui_confirm_state_t confirm;
    ls_ui_confirm_init(&confirm, HOLD_MS);
    ls_ui_confirm_effect_t eff = ls_ui_confirm_step(&confirm,
                                                    LS_UI_CONFIRM_PRESS, 0);
    ls_safe_hint_update(&hint, eff, PRIMARY,
                        ls_ui_confirm_remaining_ms(&confirm, 0));

    /* LVGL emits LV_EVENT_PRESSING every refresh. Two updates inside the same
       tenth must produce one repaint, not thirty a second. */
    eff = ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_UPDATE, 210);
    LS_CHECK(ls_safe_hint_update(&hint, eff, PRIMARY,
                                 ls_ui_confirm_remaining_ms(&confirm, 210)));
    eff = ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_UPDATE, 230);
    LS_CHECK(!ls_safe_hint_update(&hint, eff, PRIMARY,
                                  ls_ui_confirm_remaining_ms(&confirm, 230)));
    eff = ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_UPDATE, 320);
    LS_CHECK(ls_safe_hint_update(&hint, eff, PRIMARY,
                                 ls_ui_confirm_remaining_ms(&confirm, 320)));
}

/* ------------------------------------------------------------ refusals --- */

LS_CASE(a_tap_is_refused_and_says_so_instead_of_going_silent)
{
    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, HOLD_MS);

    char idle[LS_SAFE_HINT_TEXT_MAX];
    render(&hint, idle);

    gesture(&hint, PRIMARY, 5000, 5120, 20);

    LS_EQ_INT(hint.phase, LS_SAFE_HINT_CANCELLED);

    char text[LS_SAFE_HINT_TEXT_MAX];
    render(&hint, text);
    LS_CHECK_MSG(strstr(text, PRIMARY) != NULL, "cancelled hint: '%s'", text);
    LS_CHECK_MSG(strstr(text, "CANCELLED") != NULL, "cancelled hint: '%s'", text);

    LS_CHECK_MSG(strstr(text, "1.5 s") != NULL, "cancelled hint: '%s'", text);
    /* And it must be distinguishable from the untouched screen, or the tap
       still looks like it did nothing. */
    LS_CHECK_MSG(strcmp(text, idle) != 0, "cancelled hint equals idle");
    LS_EQ_INT(ls_safe_hint_color(&hint), LS_UI_COLOR_ALARM);
}

LS_CASE(pressing_again_after_a_refusal_returns_to_the_countdown)
{
    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, HOLD_MS);
    gesture(&hint, PRIMARY, 0, 100, 20);
    LS_EQ_INT(hint.phase, LS_SAFE_HINT_CANCELLED);

    ls_ui_confirm_state_t confirm;
    ls_ui_confirm_init(&confirm, HOLD_MS);
    ls_ui_confirm_effect_t eff = ls_ui_confirm_step(&confirm,
                                                    LS_UI_CONFIRM_PRESS, 500);
    LS_CHECK(ls_safe_hint_update(&hint, eff, SECONDARY,
                                 ls_ui_confirm_remaining_ms(&confirm, 500)));
    LS_EQ_INT(hint.phase, LS_SAFE_HINT_COUNTDOWN);

    /* The second control's caption, not the first one's. */
    char text[LS_SAFE_HINT_TEXT_MAX];
    render(&hint, text);
    LS_CHECK_MSG(strstr(text, SECONDARY) != NULL, "retry hint: '%s'", text);
    LS_CHECK_MSG(strstr(text, PRIMARY) == NULL, "retry hint: '%s'", text);
}

LS_CASE(a_drag_off_the_button_reads_as_cancelled_too)
{
    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, HOLD_MS);

    ls_ui_confirm_state_t confirm;
    ls_ui_confirm_init(&confirm, HOLD_MS);
    ls_ui_confirm_effect_t eff = ls_ui_confirm_step(&confirm,
                                                    LS_UI_CONFIRM_PRESS, 0);
    ls_safe_hint_update(&hint, eff, PRIMARY,
                        ls_ui_confirm_remaining_ms(&confirm, 0));

    /* LV_EVENT_PRESS_LOST - the finger slid off mid-hold. */
    eff = ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_CANCEL, 700);
    LS_EQ_INT(eff, LS_UI_CONFIRM_EFFECT_RESET);
    LS_CHECK(ls_safe_hint_update(&hint, eff, PRIMARY,
                                 ls_ui_confirm_remaining_ms(&confirm, 700)));
    LS_EQ_INT(hint.phase, LS_SAFE_HINT_CANCELLED);
}

/* ---------------------------------------------------------- completion --- */

LS_CASE(a_completed_hold_confirms)
{
    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, HOLD_MS);

    ls_ui_confirm_state_t confirm;
    ls_ui_confirm_init(&confirm, HOLD_MS);
    ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_PRESS, 0);
    ls_ui_confirm_effect_t eff = ls_ui_confirm_step(&confirm,
                                                    LS_UI_CONFIRM_UPDATE,
                                                    HOLD_MS);
    LS_EQ_INT(eff, LS_UI_CONFIRM_EFFECT_FIRE);
    LS_CHECK(ls_safe_hint_update(&hint, eff, PRIMARY,
                                 ls_ui_confirm_remaining_ms(&confirm, HOLD_MS)));
    LS_EQ_INT(hint.phase, LS_SAFE_HINT_CONFIRMED);

    char text[LS_SAFE_HINT_TEXT_MAX];
    render(&hint, text);
    LS_CHECK_MSG(strstr(text, PRIMARY) != NULL, "fired hint: '%s'", text);
    LS_CHECK_MSG(strstr(text, "CONFIRMED") != NULL, "fired hint: '%s'", text);
    LS_EQ_INT(ls_safe_hint_color(&hint), LS_UI_COLOR_ACCENT);
}

LS_CASE(the_release_that_ends_a_completed_hold_is_not_a_cancellation)
{

    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, HOLD_MS);

    ls_ui_confirm_state_t confirm;
    ls_ui_confirm_init(&confirm, HOLD_MS);
    ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_PRESS, 0);
    ls_ui_confirm_effect_t eff = ls_ui_confirm_step(&confirm,
                                                    LS_UI_CONFIRM_UPDATE, 1600);
    LS_EQ_INT(eff, LS_UI_CONFIRM_EFFECT_FIRE);
    ls_safe_hint_update(&hint, eff, PRIMARY,
                        ls_ui_confirm_remaining_ms(&confirm, 1600));

    eff = ls_ui_confirm_step(&confirm, LS_UI_CONFIRM_RELEASE, 1700);
    LS_EQ_INT(eff, LS_UI_CONFIRM_EFFECT_RESET);
    LS_CHECK(!ls_safe_hint_update(&hint, eff, PRIMARY,
                                  ls_ui_confirm_remaining_ms(&confirm, 1700)));
    LS_EQ_INT(hint.phase, LS_SAFE_HINT_CONFIRMED);

    char text[LS_SAFE_HINT_TEXT_MAX];
    render(&hint, text);
    LS_CHECK_MSG(strstr(text, "CANCELLED") == NULL, "post-fire hint: '%s'", text);
}

/* --------------------------------------------------------------- fit ----- */

LS_CASE(every_phase_fits_the_narrowest_panel)
{
    /* 480 px is the smallest panel in bench/configs.json; the hint has to fit
       on one line there or it wraps and pushes the buttons off the bottom. */
    static const char *actions[] = {
        PRIMARY, SECONDARY,
        "ERASE EVERY SETTING",           /* 19 - the caption budget */
        "A CAPTION FAR LONGER THAN FITS" /* truncated on the way in */
    };

    for (unsigned a = 0; a < sizeof(actions) / sizeof(actions[0]); a++) {
        for (int p = LS_SAFE_HINT_READY; p <= LS_SAFE_HINT_CONFIRMED; p++) {
            ls_safe_hint_t hint;
            ls_safe_hint_init(&hint, HOLD_MS);
            hint.phase = (ls_safe_hint_phase_t)p;
            hint.remaining_ms = 100;
            /* Through the public setter, so the caption bound is the one the
               screen actually applies. */
            ls_safe_hint_update(&hint, LS_UI_CONFIRM_EFFECT_PROGRESS,
                                actions[a], 100);
            hint.phase = (ls_safe_hint_phase_t)p;

            char text[LS_SAFE_HINT_TEXT_MAX];
            size_t n = ls_safe_hint_format(&hint, text, sizeof(text));
            LS_CHECK_MSG(n <= LS_SAFE_HINT_MAX_CHARS,
                         "phase %d action '%s' rendered %u chars: '%s'",
                         p, actions[a], (unsigned)n, text);
        }
    }
}

LS_CASE(an_absurd_hold_cannot_overflow_the_line)
{

    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, 0xFFFFFFFFu);
    ls_safe_hint_update(&hint, LS_UI_CONFIRM_EFFECT_PROGRESS,
                        "ERASE EVERY SETTING", 0xFFFFFFFFu);

    char text[LS_SAFE_HINT_TEXT_MAX];
    for (int p = LS_SAFE_HINT_READY; p <= LS_SAFE_HINT_CONFIRMED; p++) {
        hint.phase = (ls_safe_hint_phase_t)p;
        size_t n = ls_safe_hint_format(&hint, text, sizeof(text));
        LS_CHECK_MSG(n <= LS_SAFE_HINT_MAX_CHARS,
                     "phase %d rendered %u chars: '%s'", p, (unsigned)n, text);
    }
}

LS_CASE(null_and_tiny_buffers_are_survivable)
{
    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, HOLD_MS);

    char one[1] = { 'x' };
    LS_EQ_UINT((unsigned)ls_safe_hint_format(&hint, one, sizeof(one)), 0u);
    LS_EQ_INT(one[0], '\0');

    char small[8];
    memset(small, 'x', sizeof(small));
    size_t n = ls_safe_hint_format(&hint, small, sizeof(small));
    LS_CHECK(n < sizeof(small));
    LS_EQ_INT(small[sizeof(small) - 1], '\0');

    LS_EQ_UINT((unsigned)ls_safe_hint_format(NULL, small, sizeof(small)), 0u);
    LS_EQ_UINT((unsigned)ls_safe_hint_format(&hint, NULL, 16), 0u);
    LS_CHECK(!ls_safe_hint_update(NULL, LS_UI_CONFIRM_EFFECT_FIRE, PRIMARY, 0));
    LS_EQ_INT(ls_safe_hint_color(NULL), LS_UI_COLOR_DIM_TEXT);

    /* A NULL caption must not leave the previous action's name behind. */
    ls_safe_hint_update(&hint, LS_UI_CONFIRM_EFFECT_PROGRESS, PRIMARY, 500);
    ls_safe_hint_update(&hint, LS_UI_CONFIRM_EFFECT_PROGRESS, NULL, 500);
    char text[LS_SAFE_HINT_TEXT_MAX];
    render(&hint, text);
    LS_CHECK_MSG(strstr(text, PRIMARY) == NULL, "null-caption hint: '%s'", text);
}
