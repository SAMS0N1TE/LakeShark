#include "ls_test.h"
#include "settings/settings_action.h"
#include "settings/settings_wifi_lifecycle.h"
#include "ui/ls_ui_logic.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

LS_CASE(wifi_close_releases_worker_only_after_outstanding_command)
{
    settings_wifi_lifecycle_t life = {true, false};
    LS_CHECK(settings_wifi_claim_worker(&life));
    life.active = false;
    LS_CHECK(!settings_wifi_retire_worker(&life, true));
    LS_CHECK(life.running);
    LS_CHECK(settings_wifi_retire_worker(&life, false));
    LS_CHECK(!life.running);
}

LS_CASE(wifi_reopen_and_allocation_retry_claim_exactly_one_worker)
{
    settings_wifi_lifecycle_t life = {true, false};
    LS_CHECK(settings_wifi_claim_worker(&life));
    LS_CHECK(!settings_wifi_claim_worker(&life));
    life.active = false;
    life.active = true; /* Reopen before the worker sees close. */
    LS_CHECK(!settings_wifi_retire_worker(&life, false));
    life.active = false;
    LS_CHECK(settings_wifi_retire_worker(&life, false));
    life.active = true; /* Reopen after retirement claims a new task. */
    LS_CHECK(settings_wifi_claim_worker(&life));
    life.running = false; /* Task allocation failed: the next refresh retries. */
    LS_CHECK(settings_wifi_claim_worker(&life));
    LS_CHECK(!settings_wifi_claim_worker(&life));
}

LS_CASE(short_press_never_confirms_and_release_resets)
{
    ls_ui_confirm_state_t state;
    ls_ui_confirm_init(&state, 3000);

    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_ARMED,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, 100));
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_PROGRESS,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_UPDATE, 2999));
    LS_EQ_UINT(101, ls_ui_confirm_remaining_ms(&state, 2999));
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_RESET,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_RELEASE, 3000));
    LS_CHECK(!state.active);
    LS_CHECK(!state.fired);
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_NONE,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_UPDATE, 9999));
}

LS_CASE(completed_hold_fires_exactly_once_despite_duplicate_events)
{
    ls_ui_confirm_state_t state;
    ls_ui_confirm_init(&state, 3000);

    ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, 50);
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_FIRE,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_UPDATE, 3050));
    LS_CHECK(state.active);
    LS_CHECK(state.fired);
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_NONE,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_UPDATE, 4050));
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_NONE,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, 4050));
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_RESET,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_RELEASE, 4051));
}

LS_CASE(cancel_and_explicit_reset_restore_a_fresh_confirmation)
{
    ls_ui_confirm_state_t state;
    ls_ui_confirm_init(&state, 2000);

    ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, 10);
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_RESET,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_CANCEL, 20));
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_ARMED,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, 30));
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_RESET,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_RESET, 31));
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_ARMED,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, 40));
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_FIRE,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_UPDATE, 2040));
}

LS_CASE(confirmation_elapsed_time_survives_tick_wrap)
{
    ls_ui_confirm_state_t state;
    ls_ui_confirm_init(&state, 32);

    ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, UINT32_MAX - 15U);
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_PROGRESS,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_UPDATE, 15));
    LS_EQ_UINT(1, ls_ui_confirm_remaining_ms(&state, 15));
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_FIRE,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_UPDATE, 16));
}

LS_CASE(toggle_feedback_pairs_new_value_with_shared_button_role)
{
    ls_settings_feedback_t feedback;

    int next = ls_settings_action_next(LS_SETTINGS_ACTION_AUTODIM, 0, 0);
    ls_settings_feedback(LS_SETTINGS_ACTION_AUTODIM, next, &feedback);
    LS_EQ_INT(1, feedback.value);
    LS_EQ_STR("ON", feedback.text);
    LS_EQ_INT(LS_BTN_TOGGLE_ON, feedback.role);

    next = ls_settings_action_next(LS_SETTINGS_ACTION_USB_AUTOREBOOT, next, 0);
    ls_settings_feedback(LS_SETTINGS_ACTION_USB_AUTOREBOOT, next, &feedback);
    LS_EQ_INT(0, feedback.value);
    LS_EQ_STR("OFF", feedback.text);
    LS_EQ_INT(LS_BTN_TOGGLE_OFF, feedback.role);

    next = ls_settings_action_next(LS_SETTINGS_ACTION_MUTE, 0, 0);
    ls_settings_feedback(LS_SETTINGS_ACTION_MUTE, next, &feedback);
    LS_EQ_STR("MUTED", feedback.text);
    LS_EQ_INT(LS_BTN_TOGGLE_ON, feedback.role);
}

LS_CASE(cycle_feedback_is_immediate_and_bounded)
{
    ls_settings_feedback_t feedback;

    LS_EQ_INT(15, ls_settings_action_next(LS_SETTINGS_ACTION_DIM_TIMEOUT, 10, 0));
    LS_EQ_INT(10, ls_settings_action_next(LS_SETTINGS_ACTION_DIM_TIMEOUT, 120, 0));
    LS_EQ_INT(10, ls_settings_action_next(LS_SETTINGS_ACTION_DIM_TIMEOUT, 17, 0));

    int next = ls_settings_action_next(LS_SETTINGS_ACTION_BOOT_SOUND, 0, 0);
    ls_settings_feedback(LS_SETTINGS_ACTION_BOOT_SOUND, next, &feedback);
    LS_EQ_INT(1, feedback.value);
    LS_EQ_STR("BEEP", feedback.text);
    LS_EQ_INT(LS_BTN_PRIMARY, feedback.role);

    next = ls_settings_action_next(LS_SETTINGS_ACTION_BOOT_SOUND, 2, 0);
    ls_settings_feedback(LS_SETTINGS_ACTION_BOOT_SOUND, next, &feedback);
    LS_EQ_INT(0, feedback.value);
    LS_EQ_STR("OFF", feedback.text);
    LS_EQ_INT(LS_BTN_TOGGLE_OFF, feedback.role);

    LS_EQ_INT(2, ls_settings_action_next(LS_SETTINGS_ACTION_THEME, 1, 3));
    LS_EQ_INT(0, ls_settings_action_next(LS_SETTINGS_ACTION_THEME, 2, 3));
}

/* ------------------------------------------- hold countdown ----- */

#define RECOVERY_HOLD_MS 1500u
/* SETTINGS_SAFETY_HOLD_MS, likewise - AppSettings.cpp is a C++ LVGL file. */
#define SETTINGS_HOLD_MS 3000u

LS_CASE(hold_caption_never_overstates_the_remaining_hold)
{

    ls_ui_confirm_state_t state;
    ls_ui_confirm_init(&state, RECOVERY_HOLD_MS);

    char cap[LS_UI_CONFIRM_CAPTION_MAX];

    /* At the press. The whole hold remains, and the whole hold is 1.5 s. */
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_ARMED,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, 1000));
    ls_ui_confirm_caption(ls_ui_confirm_remaining_tenths(&state, 1000),
                          cap, sizeof(cap));
    LS_EQ_STR(cap, "HOLD 1.5");

    /* Midpoint: 750 ms in, 750 ms left. */
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_PROGRESS,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_UPDATE, 1750));
    ls_ui_confirm_caption(ls_ui_confirm_remaining_tenths(&state, 1750),
                          cap, sizeof(cap));
    LS_EQ_STR(cap, "HOLD 0.7");

    /* One millisecond short of completion. */
    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_PROGRESS,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_UPDATE, 2499));
    LS_EQ_UINT(1, ls_ui_confirm_remaining_ms(&state, 2499));
    ls_ui_confirm_caption(ls_ui_confirm_remaining_tenths(&state, 2499),
                          cap, sizeof(cap));
    LS_EQ_STR(cap, "HOLD 0.0");

    LS_EQ_INT(LS_UI_CONFIRM_EFFECT_FIRE,
              ls_ui_confirm_step(&state, LS_UI_CONFIRM_UPDATE, 2500));

    /* The Settings destructive controls use the same caption on a 3 s hold. */
    ls_ui_confirm_state_t settings;
    ls_ui_confirm_init(&settings, SETTINGS_HOLD_MS);
    ls_ui_confirm_step(&settings, LS_UI_CONFIRM_PRESS, 0);
    ls_ui_confirm_caption(ls_ui_confirm_remaining_tenths(&settings, 0),
                          cap, sizeof(cap));
    LS_EQ_STR(cap, "HOLD 3.0");
}

LS_CASE(hold_caption_is_behind_the_clock_at_every_instant)
{

    static const uint32_t holds[] = { RECOVERY_HOLD_MS, SETTINGS_HOLD_MS,
                                      2000u, 1234u };

    for (unsigned h = 0; h < sizeof(holds) / sizeof(holds[0]); h++) {
        ls_ui_confirm_state_t state;
        ls_ui_confirm_init(&state, holds[h]);
        ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, 0);

        for (uint32_t t = 0; t < holds[h]; t++) {
            const uint32_t left = ls_ui_confirm_remaining_ms(&state, t);
            const uint32_t shown =
                ls_ui_confirm_remaining_tenths(&state, t) * 100u;
            LS_CHECK_MSG(shown <= left,
                         "hold %u ms at t=%u: caption claims %u ms, %u left",
                         (unsigned)holds[h], (unsigned)t, (unsigned)shown,
                         (unsigned)left);
        }
    }
}

LS_CASE(hold_caption_moves_through_the_whole_of_a_short_hold)
{
    /* Whole seconds is the wrong granularity below two: across 1.5 s the old
       caption changed exactly once, so for most of the hold the button gave
       no evidence it was counting at all. */
    ls_ui_confirm_state_t state;
    ls_ui_confirm_init(&state, RECOVERY_HOLD_MS);
    ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, 0);

    char cap[LS_UI_CONFIRM_CAPTION_MAX];
    char prev[LS_UI_CONFIRM_CAPTION_MAX];
    unsigned changes = 0;

    ls_ui_confirm_caption(ls_ui_confirm_remaining_tenths(&state, 0),
                          prev, sizeof(prev));
    for (uint32_t t = 1; t < RECOVERY_HOLD_MS; t++) {
        ls_ui_confirm_caption(ls_ui_confirm_remaining_tenths(&state, t),
                              cap, sizeof(cap));
        if (strcmp(cap, prev) != 0) {
            changes++;
            memcpy(prev, cap, sizeof(prev));
        }
    }
    /* 1.4 down to 0.0 - one step per tenth, none skipped and none repeated. */
    LS_EQ_UINT(15, changes);
}

LS_CASE(an_absurd_hold_clamps_the_caption_instead_of_widening_the_button)
{

    ls_ui_confirm_state_t state;
    ls_ui_confirm_init(&state, 0xFFFFFFFFu);
    ls_ui_confirm_step(&state, LS_UI_CONFIRM_PRESS, 0);

    char cap[LS_UI_CONFIRM_CAPTION_MAX];
    size_t n = ls_ui_confirm_caption(ls_ui_confirm_remaining_tenths(&state, 0),
                                     cap, sizeof(cap));
    LS_EQ_STR(cap, "HOLD 99.9");
    LS_CHECK(n < LS_UI_CONFIRM_CAPTION_MAX);
    LS_EQ_UINT(9, ls_ui_confirm_caption(0xFFFFFFFFu, cap, sizeof(cap)));
    LS_EQ_STR(cap, "HOLD 99.9");

    /* Buffer hygiene: always terminated, never written past. */
    char small[4];
    memset(small, 'x', sizeof(small));
    n = ls_ui_confirm_caption(15, small, sizeof(small));
    LS_CHECK(n < sizeof(small));
    LS_EQ_INT(small[sizeof(small) - 1], 0);

    char one[1] = { 'x' };
    LS_EQ_UINT(0, ls_ui_confirm_caption(15, one, sizeof(one)));
    LS_EQ_INT(one[0], 0);
    LS_EQ_UINT(0, ls_ui_confirm_caption(15, NULL, 8));

    /* A control nobody is touching has no countdown to show. */
    ls_ui_confirm_state_t idle;
    ls_ui_confirm_init(&idle, SETTINGS_HOLD_MS);
    LS_EQ_UINT(0, ls_ui_confirm_remaining_tenths(&idle, 500));
    LS_EQ_UINT(0, ls_ui_confirm_remaining_tenths(NULL, 500));
}
