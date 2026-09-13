#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef uint32_t nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
