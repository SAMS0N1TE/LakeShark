#pragma once
#include <stdint.h>
#include "esp_err.h"
#define NVS_KEY_NAME_MAX_SIZE 16
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_TYPE_ANY 0
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
typedef unsigned nvs_handle_t;
typedef void *nvs_iterator_t;
typedef struct { char key[NVS_KEY_NAME_MAX_SIZE]; } nvs_entry_info_t;
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_entry_find(const char *, const char *, int, nvs_iterator_t *);
esp_err_t nvs_entry_next(nvs_iterator_t *);
void nvs_entry_info(nvs_iterator_t, nvs_entry_info_t *);
void nvs_release_iterator(nvs_iterator_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_get_i8(nvs_handle_t, const char *, int8_t *);
esp_err_t nvs_set_i8(nvs_handle_t, const char *, int8_t);
esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t);
esp_err_t nvs_get_i32(nvs_handle_t, const char *, int32_t *);
esp_err_t nvs_set_i32(nvs_handle_t, const char *, int32_t);
esp_err_t nvs_get_u64(nvs_handle_t, const char *, uint64_t *);
esp_err_t nvs_set_u64(nvs_handle_t, const char *, uint64_t);
