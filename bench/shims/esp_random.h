#ifndef LS_SHIM_ESP_RANDOM_H
#define LS_SHIM_ESP_RANDOM_H

#include <stddef.h>

void esp_fill_random(void *buffer, size_t bytes);

#endif
