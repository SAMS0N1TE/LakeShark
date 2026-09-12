#pragma once
#include "esp_err.h"
/* Deliberately no unsafe parser declarations/implementations: reintroducing
   an image checker/summary/panic parser must fail this actual-adapter target. */
esp_err_t esp_core_dump_image_erase(void);
