#include "settings/settings_action.h"

#include <stddef.h>

static const int dim_steps[] = { 10, 15, 30, 60, 120 };

int ls_settings_action_next(ls_settings_action_t action, int current,
                            int value_count)
{
    switch (action) {
    case LS_SETTINGS_ACTION_MUTE:
    case LS_SETTINGS_ACTION_AUTODIM:
    case LS_SETTINGS_ACTION_USB_AUTOREBOOT:
    case LS_SETTINGS_ACTION_BLUETOOTH:
        return current ? 0 : 1;

    case LS_SETTINGS_ACTION_DIM_TIMEOUT:
        for (unsigned i = 0; i < sizeof(dim_steps) / sizeof(dim_steps[0]); ++i) {
            if (dim_steps[i] == current)
                return dim_steps[(i + 1U) % (sizeof(dim_steps) / sizeof(dim_steps[0]))];
        }
        return dim_steps[0];

    case LS_SETTINGS_ACTION_BOOT_SOUND:
        return (current >= 0 && current < 3) ? (current + 1) % 3 : 0;

    case LS_SETTINGS_ACTION_THEME:
        return value_count > 0 && current >= 0 && current < value_count
            ? (current + 1) % value_count : 0;

    default:
        return current;
    }
}

void ls_settings_feedback(ls_settings_action_t action, int value,
                          ls_settings_feedback_t *out)
{
    static const char *boot_names[] = { "OFF", "BEEP", "VOICE" };
    if (!out) return;

    out->value = value;
    out->text = NULL;
    out->role = LS_BTN_PRIMARY;

    switch (action) {
    case LS_SETTINGS_ACTION_MUTE:
        out->value = value ? 1 : 0;
        out->text = out->value ? "MUTED" : "ON";
        out->role = out->value ? LS_BTN_TOGGLE_ON : LS_BTN_TOGGLE_OFF;
        break;
    case LS_SETTINGS_ACTION_AUTODIM:
    case LS_SETTINGS_ACTION_USB_AUTOREBOOT:
    case LS_SETTINGS_ACTION_BLUETOOTH:
        out->value = value ? 1 : 0;
        out->text = out->value ? "ON" : "OFF";
        out->role = out->value ? LS_BTN_TOGGLE_ON : LS_BTN_TOGGLE_OFF;
        break;
    case LS_SETTINGS_ACTION_BOOT_SOUND:
        out->value = (value >= 0 && value < 3) ? value : 0;
        out->text = boot_names[out->value];
        out->role = out->value ? LS_BTN_PRIMARY : LS_BTN_TOGGLE_OFF;
        break;
    case LS_SETTINGS_ACTION_DIM_TIMEOUT:
    case LS_SETTINGS_ACTION_THEME:
    default:
        break;
    }
}
