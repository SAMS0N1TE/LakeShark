#include "ls_wifi_sta_core.h"

#include <string.h>
#include <stdio.h>

void ls_wifi_copy_ssid(uint8_t out[32], const char *ssid)
{
    memset(out, 0, 32);
    if (!ssid) return;
    size_t n = strlen(ssid);
    memcpy(out, ssid, n > 32 ? 32 : n);
}

void ls_wifi_connecting_status(char *out, size_t cap, const char *ssid,
                               int reason, uint32_t retry_ms)
{
    if (!out || !cap) return;
    if (reason)
        snprintf(out, cap, "Retrying \"%s\"; disconnect reason %d; %lus",
                 ssid ? ssid : "", reason, (unsigned long)(retry_ms / 1000));
    else snprintf(out, cap, "Connecting to \"%s\"", ssid ? ssid : "");
}

bool ls_wifi_ssid_valid(const char *ssid)
{
    if (!ssid) return false;
    size_t n = strlen(ssid);
    if (n == 0 || n > LS_WIFI_SSID_MAX_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)ssid[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool ls_wifi_pass_valid(const char *pass)
{
    if (!pass) return false;
    size_t n = strlen(pass);
    if (n == 0) return true;                       /* open network */
    if (n < LS_WIFI_PASS_MIN_LEN || n > LS_WIFI_PASS_MAX_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)pass[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

uint32_t ls_wifi_backoff_next(uint32_t cur, uint32_t min_ms, uint32_t max_ms)
{
    if (min_ms == 0) min_ms = 1;
    if (max_ms < min_ms) max_ms = min_ms;
    if (cur < min_ms) return min_ms;
    /* Guard against overflow before doubling. */
    if (cur > max_ms / 2) return max_ms;
    uint32_t next = cur * 2u;
    if (next > max_ms) next = max_ms;
    return next;
}
