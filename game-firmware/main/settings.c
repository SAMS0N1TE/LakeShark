#include "settings.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <math.h>
#include <string.h>

static const char *const TAG = "game_settings";
static const uint8_t defaults[] = {PAD_A, PAD_A, PAD_B, TK21_KEY, PAD_ALT, PAD_X, PAD_Y};
static const uint8_t game_bits[] = {TK21_U11, TK21_U11, TK21_U10, TK21_U5, TK21_U7, TK21_U8, TK21_U6};
static bool storage_ready;

void settings_defaults(game_settings *s)
{
    *s = (game_settings){ .version = 2, .brightness = 100, .deadzone = 13,
                          .sensitivity = 100 };
    for (int i = 0; i < BIND_COUNT; ++i) s->bindings[i] = defaults[i];
}

bool settings_calibration_valid(const stick_calibration *c)
{
    for (int i = 0; i < 2; ++i)
        if (c->high[i] > 4095 || c->center[i] < c->low[i] + 150 ||
            c->high[i] < c->center[i] + 150) return false;
    return true;
}

static bool bindable_button(uint8_t button)
{
    return button && button <= TK21_KEY && button != PAD_MENU && !(button & (button - 1));
}

static bool valid(const game_settings *s)
{
    if (s->version != 2 || s->brightness < 20 || s->brightness > 100 || s->theme >= THEME_COUNT ||
        s->deadzone > 35 || s->sensitivity < 50 || s->sensitivity > 150 || s->invert > 3)
        return false;
    if (!bindable_button(s->bindings[BIND_BOOST])) return false;
    unsigned all = 0;
    for (int i = BIND_A; i < BIND_COUNT; ++i) {
        if (!bindable_button(s->bindings[i]) || (all & s->bindings[i])) return false;
        all |= s->bindings[i];
    }
    return true;
}

static void migrate_bindings(game_settings *s)
{
    if (s->version != 1) return;
    s->version = 2;
    if (s->bindings[BIND_BOOST] == PAD_MENU) s->bindings[BIND_BOOST] = PAD_A;
    if (memcmp(s->bindings + BIND_A, game_bits + BIND_A, BIND_COUNT - BIND_A) == 0) {
        memcpy(s->bindings + BIND_A, defaults + BIND_A, BIND_COUNT - BIND_A);
    } else {
        for (int i = BIND_A; i < BIND_COUNT; ++i)
            if (s->bindings[i] == PAD_MENU) s->bindings[i] = TK21_KEY;
    }
}

void settings_load(game_settings *s)
{
    settings_defaults(s);
    esp_err_t error = nvs_flash_init();
    storage_ready = error == ESP_OK;
    if (!storage_ready) {
        ESP_LOGW(TAG, "NVS settings unavailable: %s", esp_err_to_name(error));
        return;
    }
    nvs_handle_t store;
    if (nvs_open("lsgame", NVS_READONLY, &store) != ESP_OK) return;
    game_settings saved;
    size_t length = sizeof(saved);
    error = nvs_get_blob(store, "profile", &saved, &length);
    nvs_close(store);
    if (error == ESP_OK && length == sizeof(saved)) {
        migrate_bindings(&saved);
        if (valid(&saved)) *s = saved;
    }
}

esp_err_t settings_save(const game_settings *s)
{
    if (!storage_ready) return ESP_ERR_INVALID_STATE;
    if (!valid(s)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t store;
    esp_err_t error = nvs_open("lsgame", NVS_READWRITE, &store);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(store, "profile", s, sizeof(*s));
    if (error == ESP_OK) error = nvs_commit(store);
    nvs_close(store);
    return error;
}

void settings_bind(game_settings *s, int action, uint8_t button)
{
    if (action < 0 || action >= BIND_COUNT || !bindable_button(button)) return;
    if (action >= BIND_A) {
        for (int i = BIND_A; i < BIND_COUNT; ++i)
            if (s->bindings[i] == button) {
                s->bindings[i] = s->bindings[action];
                break;
            }
    }
    s->bindings[action] = button;
}

static float normalized_axis(const game_settings *s, const tk21_raw_state *raw, int axis)
{
    const bool calibrated = settings_calibration_valid(&s->calibration);
    int center = calibrated ? s->calibration.center[axis] : raw->center[axis];
    int high = calibrated ? s->calibration.high[axis] : center + (4095 - center) * 78 / 100;
    int low = calibrated ? s->calibration.low[axis] : center - center * 78 / 100;
    int delta = raw->axis[axis] - center;
    int range = delta >= 0 ? high - center : center - low;
    float fraction = range ? (float)delta / range : 0;
    float deadzone = s->deadzone * 0.01f;
    float active = fmaxf(0, fabsf(fraction) - deadzone) / (1 - deadzone);
    float value = copysignf(fminf(1, active * s->sensitivity * 0.01f), fraction);
    return (s->invert & (1u << axis)) ? -value : value;
}

tk21_state settings_input(const game_settings *s, tk21_state in, tk21_raw_state raw)
{
    in.x = normalized_axis(s, &raw, 0);
    in.y = normalized_axis(s, &raw, 1);
    return in;
}

tk21_state settings_game_buttons(const game_settings *s, tk21_state in, bool gba)
{
    uint8_t held = 0, pressed = 0;
    int first = gba ? BIND_A : BIND_BOOST;
    int end = gba ? BIND_COUNT : BIND_A;
    for (int i = first; i < end; ++i) {
        if (in.held & s->bindings[i]) held |= game_bits[i];
        if (in.pressed & s->bindings[i]) pressed |= game_bits[i];
    }
    in.held = held;
    in.pressed = pressed;
    return in;
}

const char *settings_button_name(uint8_t button)
{
    static const char *const labels[] = {"Menu", "Alt", "A", "X", "Y", "B", "Stick"};
    for (int i = 0; i < 7; ++i)
        if (button == (1u << i)) return labels[i];
    return "-";
}
