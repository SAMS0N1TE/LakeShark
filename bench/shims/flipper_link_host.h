/* Declarations supplied by ESP-IDF's libc integration on target. */
#ifndef LS_SHIM_FLIPPER_LINK_HOST_H
#define LS_SHIM_FLIPPER_LINK_HOST_H

#include <stddef.h>

#define CONFIG_ESP_CONSOLE_UART_NUM 0

size_t strlcpy(char *dst, const char *src, size_t size);

#endif
