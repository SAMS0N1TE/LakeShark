/* Production LINK renderer, simulated worker snapshot; no radio or audio. */
#include "ls_wireless.h"
#include <stdio.h>
#include <string.h>

static ls_wireless_snapshot_t s_model;
static bool s_seeded;

static void seed(void)
{
    if (s_seeded) return;
    s_seeded = true;
    s_model.wifi_available = s_model.wifi_connected = s_model.wifi_signal = true;
    s_model.now_ms = s_model.scan_ms = 60000;
    s_model.scan_revision = 1;
    s_model.saved_count = 8;
    s_model.ap_count = 12;
    snprintf(s_model.ssid, 33, "Home Net");
    snprintf(s_model.ip, sizeof(s_model.ip), "192.168.1.42");
    snprintf(s_model.wifi_status, sizeof(s_model.wifi_status), "Connected: Home Net");
    snprintf(s_model.message, sizeof(s_model.message), "Select saved / FORGET removes one / * connected");
    s_model.wifi_rssi = -42; s_model.channel = 6;
    for (int i = 0; i < 8; ++i)
        snprintf(s_model.saved[i], 33, i == 0 ? "Home Net" : i == 1 ? "Office Net" : "Saved site %d", i);
    for (int i = 0; i < 12; ++i) {
        snprintf(s_model.aps[i].ssid, 33, i == 0 ? "Home Net" : i == 1 ? "Office Net" : "Nearby %d", i);
        s_model.aps[i].rssi = -42 - i * 3;
        s_model.aps[i].channel = i % 11 + 1;
        s_model.aps[i].secure = true;
        s_model.aps[i].saved = i < 2;
    }
    for (int i = 0; i < 60; ++i)
        ls_wireless_history_push(&s_model.history, i * 1000, -42 - i % 5,
                                true, 0, false, false, 0, 0);
}

void ls_wireless_get(ls_wireless_snapshot_t *out) { seed(); *out = s_model; }
void ls_wireless_set_active(bool active) { (void)active; seed(); }
void ls_wireless_observe(bool active) { (void)active; }
bool ls_wireless_request(ls_wireless_op_t op, const char *ssid, const char *pass)
{
    (void)pass;
    seed();
    if (op == LS_WIRELESS_FORGET && ssid) {
        for (int i = 0; i < s_model.saved_count; ++i) {
            if (strcmp(ssid, s_model.saved[i])) continue;
            for (int j = i; j + 1 < s_model.saved_count; ++j)
                memcpy(s_model.saved[j], s_model.saved[j + 1], 33);
            s_model.saved_count--;
            break;
        }
    }
    snprintf(s_model.message, sizeof(s_model.message), "Simulated request %d%s%s",
             op, ssid ? ": " : "", ssid ? ssid : "");
    return true;
}
