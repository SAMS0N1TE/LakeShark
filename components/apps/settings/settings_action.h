#pragma once

#include "ui/ls_ui_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_SETTINGS_ACTION_MUTE = 0,
    LS_SETTINGS_ACTION_AUTODIM,
    LS_SETTINGS_ACTION_DIM_TIMEOUT,
    LS_SETTINGS_ACTION_BOOT_SOUND,
    LS_SETTINGS_ACTION_THEME,
    LS_SETTINGS_ACTION_USB_AUTOREBOOT,
    /*LS-785*/
    LS_SETTINGS_ACTION_BLUETOOTH
} ls_settings_action_t;

typedef struct {
    int value;
    const char *text;
    ls_ui_button_role_t role;
} ls_settings_feedback_t;

int ls_settings_action_next(ls_settings_action_t action, int current,
                            int value_count);
void ls_settings_feedback(ls_settings_action_t action, int value,
                          ls_settings_feedback_t *out);

#ifdef __cplusplus
}
#endif
