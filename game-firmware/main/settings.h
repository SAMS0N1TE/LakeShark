#pragma once
#include "tk21.h"

enum { BIND_BOOST, BIND_A, BIND_B, BIND_START, BIND_SELECT, BIND_L, BIND_R, BIND_COUNT };
enum { THEME_CYAN, THEME_AMBER, THEME_GREEN, THEME_COUNT };
enum { PAD_A = TK21_U5, PAD_B = TK21_U6, PAD_X = TK21_U7, PAD_Y = TK21_U8,
       PAD_MENU = TK21_U11, PAD_ALT = TK21_U10 };

typedef struct {
    uint16_t low[2], center[2], high[2];
} stick_calibration;

typedef struct {
    uint8_t version, brightness, theme, deadzone, sensitivity, invert;
    uint8_t bindings[BIND_COUNT];
    stick_calibration calibration;
} game_settings;

void settings_load(game_settings *settings);
esp_err_t settings_save(const game_settings *settings);
void settings_defaults(game_settings *settings);
void settings_bind(game_settings *settings, int action, uint8_t button);
bool settings_calibration_valid(const stick_calibration *calibration);
tk21_state settings_input(const game_settings *settings, tk21_state physical, tk21_raw_state raw);
tk21_state settings_game_buttons(const game_settings *settings, tk21_state input, bool gba);
const char *settings_button_name(uint8_t button);
