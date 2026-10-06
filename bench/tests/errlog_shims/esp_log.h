/* The log as the error log's hook sees it: lines formatted as the IDF
   formats them with colours off, through a vprintf that can be replaced.
   Bench only. */
#ifndef LS_ERRLOG_SHIM_ESP_LOG_H
#define LS_ERRLOG_SHIM_ESP_LOG_H

#include <stdarg.h>
#include <stdio.h>

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

typedef int (*vprintf_like_t)(const char *, va_list);

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...);

#define LS_ERRLOG_SHIM_LINE(letter, lvl, tag, fmt, ...) \
    esp_log_write(lvl, tag, letter " (%lu) %s: " fmt "\n", 0ul, tag, ##__VA_ARGS__)

#define ESP_LOGE(tag, fmt, ...) LS_ERRLOG_SHIM_LINE("E", ESP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) LS_ERRLOG_SHIM_LINE("W", ESP_LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) LS_ERRLOG_SHIM_LINE("I", ESP_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) LS_ERRLOG_SHIM_LINE("D", ESP_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)

#endif
