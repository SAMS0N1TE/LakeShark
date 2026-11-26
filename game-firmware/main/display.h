#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifndef LAKESHARK_RENDER_WIDTH
#define LAKESHARK_RENDER_WIDTH 400
#define LAKESHARK_RENDER_HEIGHT 240
#endif
enum { GAME_WIDTH = LAKESHARK_RENDER_WIDTH, GAME_HEIGHT = LAKESHARK_RENDER_HEIGHT };

esp_err_t game_display_init(void);
esp_err_t game_display_brightness(unsigned percent);
esp_err_t game_display_blank_for_restart(void);
/* Get the next writable frame. Its ownership passes to the presenter on submit. */
uint16_t *game_display_buffer(void);
esp_err_t game_display_present(void);
/* Wait for the last submitted buffer to complete a full LCD refresh. */
esp_err_t game_display_wait_visible(void);
unsigned game_display_scale_us(void);
