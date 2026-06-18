
#ifndef BSOD_H
#define BSOD_H

#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void bsod_init(esp_lcd_panel_handle_t dsi_panel);

#ifdef __cplusplus
}
#endif

#endif
