#pragma once

#include <stdbool.h>
#include "../../../main/ls_wifi.h"

enum SettingsWifiCommand { WIFI_IDLE, WIFI_SCAN, WIFI_JOIN, WIFI_LEAVE,
                           WIFI_FORGET, WIFI_REJOIN };
struct SettingsWifiSnapshot {
    char status[160];
    char result[80];
    bool busy;
    unsigned scan_revision;
    int count;
    ls_wifi_scan_ap_t networks[16];
};

bool settings_wifi_start();
void settings_wifi_active(bool active);
bool settings_wifi_stopped();
void settings_wifi_snapshot(SettingsWifiSnapshot *out);
bool settings_wifi_request(SettingsWifiCommand command,
                           const char *ssid = nullptr, const char *password = nullptr);
