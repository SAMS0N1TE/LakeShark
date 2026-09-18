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

const char *ls_wifi_reason_text(int reason)
{
    switch (reason) {
    case 8:   return "association left";
    case 15:  return "4-way handshake timeout";
    case 200: return "beacon timeout";
    case 201: return "network not found";
    case 202: return "authentication failed";
    case 203: return "association failed";
    case 204: return "handshake timeout";
    case 205: return "connection failed";
    case 210: return "incompatible security";
    case 211: return "authentication mode rejected";
    case 212: return "signal below threshold";
    default:  return NULL;
    }
}

void ls_wifi_connecting_status(char *out, size_t cap, const char *ssid,
                               int reason, uint32_t retry_ms)
{
    if (!out || !cap) return;
    if (reason) {
        const char *why = ls_wifi_reason_text(reason);
        if (why)
            snprintf(out, cap, "Retrying \"%s\"; %s (%d); %lus",
                     ssid ? ssid : "", why, reason,
                     (unsigned long)(retry_ms / 1000));
        else
            snprintf(out, cap, "Retrying \"%s\"; disconnect reason %d; %lus",
                     ssid ? ssid : "", reason,
                     (unsigned long)(retry_ms / 1000));
    } else snprintf(out, cap, "Connecting to \"%s\"", ssid ? ssid : "");
}

bool ls_wifi_should_retry(int attempts, int reason)
{
    if (attempts < 0) return true;
    /* 201 is "network not found": the AP is not in range, and asking a
       fifth time does not put it there. */
    if (reason == 201) return attempts < LS_WIFI_TRIES_NOT_FOUND;
    return attempts < LS_WIFI_TRIES_OTHER;
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
