/* Host shim. Never compiled into firmware. */
#ifndef LS_SHIM_ESP_LOG_H
#define LS_SHIM_ESP_LOG_H

#include <stdio.h>

extern int ls_shim_log_enabled;

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

esp_log_level_t esp_log_level_get(const char *tag);
void esp_log_level_set(const char *tag, esp_log_level_t level);

#define LS_SHIM_LOG(lvl, tag, fmt, ...)                                     \
    do { if (ls_shim_log_enabled)                                           \
             fprintf(stderr, "[" lvl "] %s: " fmt "\n", tag, ##__VA_ARGS__); \
    } while (0)

#define ESP_LOGE(tag, fmt, ...) LS_SHIM_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) LS_SHIM_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) LS_SHIM_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) LS_SHIM_LOG("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) LS_SHIM_LOG("V", tag, fmt, ##__VA_ARGS__)

#define ESP_LOG_BUFFER_HEX(tag, buf, len)      ((void)0)
#define ESP_LOG_BUFFER_HEXDUMP(t, b, l, lvl)   ((void)0)

#endif
