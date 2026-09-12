/* Host shim.  file_iterator.c uses the ESP_RETURN_ON_* helpers from
   esp_check.h; strip them to plain-C conditionals so the bench can compile
   the vendored source unchanged. */
#ifndef LS_SHIM_ESP_CHECK_H
#define LS_SHIM_ESP_CHECK_H

#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"

#define ESP_RETURN_ON_FALSE(cond, err, tag, fmt, ...)                       \
    do { if (!(cond)) {                                                     \
        ESP_LOGE(tag, fmt, ##__VA_ARGS__);                                  \
        return (err);                                                       \
    } } while (0)

#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...)                               \
    do { esp_err_t _rc = (x);                                               \
         if (_rc != ESP_OK) {                                               \
             ESP_LOGE(tag, fmt, ##__VA_ARGS__);                             \
             return _rc;                                                    \
         } } while (0)

#endif
