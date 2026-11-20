#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct esp_http_client *esp_http_client_handle_t;
#ifndef ESP_ERR_HTTP_EAGAIN
#define ESP_ERR_HTTP_EAGAIN 0x7007
#endif
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
