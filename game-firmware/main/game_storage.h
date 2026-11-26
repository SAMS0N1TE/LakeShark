#pragma once
#include "waveshare_p4_sd.h"

static inline esp_err_t game_storage_mount(void)
{
    return waveshare_p4_sd_mount("/sd", 5);
}
