/* Compile-time transport seam; host tests supply deterministic responses. */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
typedef struct esp_http_client *esp_http_client_handle_t;
int esp_http_client_read(esp_http_client_handle_t, char *, int);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
