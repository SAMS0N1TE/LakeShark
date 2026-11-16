/* Names for esp_err_t codes when ESP-IDF's own table is compiled out.

   The T-Display-P4 image sets CONFIG_ESP_ERR_TO_NAME_LOOKUP=n for app
   space, and IDF's esp_err_to_name() then answers "UNKNOWN ERROR" for every
   code, ESP_OK included: the console printed "gps: start UNKNOWN ERROR" for
   a start that worked. esp_err_to_name is wrapped at link time (see this
   component's CMakeLists) to answer from the small table here instead: the
   common codes by name, anything else as "ERR 0x...". */
#ifndef LS_ERR_NAME_H
#define LS_ERR_NAME_H
#include <stddef.h>
#include <stdint.h>

/* The name of code; a code with no name is written into buf (at least 16
   bytes) as "ERR 0x%x" and buf is returned. */
const char *ls_err_name(int32_t code, char *buf, size_t cap);

#endif
