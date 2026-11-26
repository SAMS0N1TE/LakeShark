#pragma once
#include "settings.h"

typedef enum {
    MENU_NONE, MENU_RELAY, MENU_SALVAGE, MENU_GBA_DEMO, MENU_GBA_SD,
    MENU_RESUME, MENU_RESTART, MENU_HOME
} menu_action;

typedef enum { NAV_NONE, NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT } menu_direction;

void menu_home(void);
void menu_pause(menu_action running);
void menu_notice(const char *text);
menu_action menu_update(game_settings *settings, tk21_state physical, tk21_raw_state raw,
                        menu_direction serial, int64_t now);
void menu_draw(uint16_t *frame, const game_settings *settings, tk21_raw_state raw,
               tk21_state physical, int64_t now);
void menu_log(const game_settings *settings);
void menu_text(uint16_t *frame, int x, int y, const char *text, uint16_t color);
