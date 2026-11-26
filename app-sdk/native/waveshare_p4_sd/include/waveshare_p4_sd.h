#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the Waveshare 4.3-inch P4 board's onboard slot for the app lifetime.
   Call from one task before using files. Calling again at the same path is
   harmless. A mount failure never formats the card. */
esp_err_t waveshare_p4_sd_mount(const char *base_path, int max_files);

#ifdef __cplusplus
}
#endif
