#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t x, y;
    uint8_t id;
    bool down;
} waveshare_p4_touch_sample;

esp_err_t waveshare_p4_touch_init(void);
/* Reports a single contact or a release. ESP_ERR_NOT_FOUND means no new report. */
esp_err_t waveshare_p4_touch_read(waveshare_p4_touch_sample *sample);
