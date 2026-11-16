/* See ls_err_name.h. */
#include "ls_err_name.h"
#include <stdio.h>

static const struct { int32_t code; const char *name; } NAMES[] = {
    { 0, "ESP_OK" },
    { -1, "ESP_FAIL" },
    { 0x101, "ESP_ERR_NO_MEM" },
    { 0x102, "ESP_ERR_INVALID_ARG" },
    { 0x103, "ESP_ERR_INVALID_STATE" },
    { 0x104, "ESP_ERR_INVALID_SIZE" },
    { 0x105, "ESP_ERR_NOT_FOUND" },
    { 0x106, "ESP_ERR_NOT_SUPPORTED" },
    { 0x107, "ESP_ERR_TIMEOUT" },
    { 0x108, "ESP_ERR_INVALID_RESPONSE" },
    { 0x109, "ESP_ERR_INVALID_CRC" },
    { 0x10A, "ESP_ERR_INVALID_VERSION" },
    { 0x10B, "ESP_ERR_INVALID_MAC" },
    { 0x10C, "ESP_ERR_NOT_FINISHED" },
    { 0x10D, "ESP_ERR_NOT_ALLOWED" },
    { 0x1101, "ESP_ERR_NVS_NOT_INITIALIZED" },
    { 0x1102, "ESP_ERR_NVS_NOT_FOUND" },
    { 0x110D, "ESP_ERR_NVS_NO_FREE_PAGES" },
};

const char *ls_err_name(int32_t code, char *buf, size_t cap)
{
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (NAMES[i].code == code) return NAMES[i].name;
    if (buf && cap) snprintf(buf, cap, "ERR 0x%lx", (unsigned long)(uint32_t)code);
    return buf ? buf : "ERR";
}

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if !defined(CONFIG_ESP_ERR_TO_NAME_LOOKUP)
#include "esp_err.h"
/* Every caller in the image reaches this instead of IDF's (link --wrap).
   Unnamed codes go to one of a few rotating buffers: callers print the
   result at once, and four in flight is more than any line uses. */
const char *__wrap_esp_err_to_name(esp_err_t code);
const char *__wrap_esp_err_to_name(esp_err_t code)
{
    static char ring[4][16];
    static unsigned at;
    char *buf = ring[at++ & 3u];
    return ls_err_name((int32_t)code, buf, sizeof(ring[0]));
}
#endif
#endif
