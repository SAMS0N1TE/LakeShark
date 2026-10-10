/* NVS seam for SWEEP persistence tests, not a firmware header. */
#ifndef SWEEP_TEST_NVS_H
#define SWEEP_TEST_NVS_H
#include "esp_err.h"
#include <stddef.h>
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
#endif
