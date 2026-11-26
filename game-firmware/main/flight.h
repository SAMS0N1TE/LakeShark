#pragma once
#include "esp_err.h"
#include "tk21.h"
#include <stdint.h>

esp_err_t flight_init(void);
void flight_start(bool salvage);
void flight_update(tk21_state input, float dt);
void flight_draw(uint16_t *pixels);
void flight_log(void);
