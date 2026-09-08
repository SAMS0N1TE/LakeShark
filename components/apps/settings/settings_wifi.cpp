#include "settings_wifi.h"
#include "settings_wifi_lifecycle.h"
#include "../../../main/ls_wifi_sta_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstdio>
#include <cstring>

namespace {
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
StaticSemaphore_t wake_storage;
SemaphoreHandle_t wake = nullptr;
settings_wifi_lifecycle_t life = {};
SettingsWifiSnapshot state = {};
SettingsWifiCommand pending = WIFI_IDLE;
char pending_ssid[33];
char pending_password[64];

void wipe(char *p, size_t n) { volatile char *v = p; while (n--) *v++ = 0; }

/* LS-761: scans block in esp_wifi_scan_start and credentials touch NVS.
 * Keep both off LVGL. The worker owns no screen pointers, so closing Settings
 * during a scan cannot deliver a result into a deleted widget. Its internal
 * task stack remains valid during flash/NVS cache-disabled operations.
 * A process-lifetime static semaphore wakes a short-lived worker; notifying a
 * saved task handle would race its deletion on close. The idle worker retires
 * after the final command, returning its 6 KiB internal stack to FreeRTOS. */
void run(void *)
{
    for (;;) {
        char ssid[33], password[64];
        portENTER_CRITICAL(&mux);
        if (settings_wifi_retire_worker(&life, state.busy)) {
            portEXIT_CRITICAL(&mux);
            vTaskDelete(nullptr);
            return;
        }
        SettingsWifiCommand command = pending;
        pending = WIFI_IDLE;
        memcpy(ssid, pending_ssid, sizeof(ssid));
        memcpy(password, pending_password, sizeof(password));
        wipe(pending_password, sizeof(pending_password));
        portEXIT_CRITICAL(&mux);
        esp_err_t error = ESP_OK;
        int count = 0;
        ls_wifi_scan_ap_t networks[16] = {};
        const char *result = nullptr;
        switch (command) {
        case WIFI_SCAN:
            count = ls_wifi_sta_scan(networks, 16);
            if (count < 0) { error = ESP_FAIL; count = 0; }
            result = count ? "Select a network" : "No networks found";
            break;
        case WIFI_JOIN: error = ls_wifi_sta_join(ssid, password); result = "Credentials saved"; break;
        case WIFI_LEAVE: error = ls_wifi_sta_leave(); result = "Disconnected; network retained"; break;
        case WIFI_FORGET: error = ls_wifi_sta_forget(); result = "Saved network forgotten"; break;
        case WIFI_REJOIN: error = ls_wifi_sta_autojoin(); result = "Connecting to saved network"; break;
        default: break;
        }
        wipe(password, sizeof(password));
        char status[160];
        ls_wifi_sta_status(status, sizeof(status));
        portENTER_CRITICAL(&mux);
        memcpy(state.status, status, sizeof(status));
        if (command != WIFI_IDLE) {
            if (error != ESP_OK)
                snprintf(state.result, sizeof(state.result), "%s failed: %s",
                         command == WIFI_SCAN ? "Scan" : "Wi-Fi", esp_err_to_name(error));
            else snprintf(state.result, sizeof(state.result), "%s", result);
            if (command == WIFI_SCAN) {
                state.count = count;
                memcpy(state.networks, networks, sizeof(networks));
                ++state.scan_revision;
            }
            state.busy = false;
        }
        bool retire = settings_wifi_retire_worker(&life, state.busy);
        portEXIT_CRITICAL(&mux);
        if (retire) { vTaskDelete(nullptr); return; }
        xSemaphoreTake(wake, pdMS_TO_TICKS(1000));
    }
}
}

bool settings_wifi_start()
{
    if (!wake) wake = xSemaphoreCreateBinaryStatic(&wake_storage);
    portENTER_CRITICAL(&mux);
    life.active = true;
    bool create = settings_wifi_claim_worker(&life);
    portEXIT_CRITICAL(&mux);
    /* IDF 5.5 heap_idf.c maps pvPortMalloc (including dynamic task stacks)
     * explicitly to MALLOC_CAP_INTERNAL|8BIT, even when external static stacks
     * are allowed. Use the dynamic API so self-deletion requires no helper-task
     * allocation (vTaskDeleteWithCaps would allocate, and can abort on low RAM). */
    if (create && xTaskCreate(run, "settings_wifi", 6144, nullptr, 5, nullptr) != pdPASS) {
        portENTER_CRITICAL(&mux); life.running = false; portEXIT_CRITICAL(&mux);
        return false;
    }
    return true;
}
void settings_wifi_active(bool enabled)
{
    if (enabled) { (void)settings_wifi_start(); return; }
    portENTER_CRITICAL(&mux); life.active = false; portEXIT_CRITICAL(&mux);
    if (wake) xSemaphoreGive(wake);
}
void settings_wifi_snapshot(SettingsWifiSnapshot *out)
{
    portENTER_CRITICAL(&mux); *out = state; portEXIT_CRITICAL(&mux);
}
bool settings_wifi_stopped()
{
    portENTER_CRITICAL(&mux);
    bool stopped = !life.running;
    portEXIT_CRITICAL(&mux);
    return stopped;
}
bool settings_wifi_request(SettingsWifiCommand command, const char *ssid, const char *password)
{
    if (command == WIFI_IDLE) return false;
    if (command == WIFI_JOIN && (!ls_wifi_ssid_valid(ssid) ||
                                 !ls_wifi_pass_valid(password ? password : ""))) return false;
    if (!settings_wifi_start()) return false;
    portENTER_CRITICAL(&mux);
    if (state.busy) { portEXIT_CRITICAL(&mux); return false; }
    pending = command;
    snprintf(pending_ssid, sizeof(pending_ssid), "%s", ssid ? ssid : "");
    snprintf(pending_password, sizeof(pending_password), "%s", password ? password : "");
    state.busy = true;
    snprintf(state.result, sizeof(state.result), "%s", command == WIFI_SCAN ? "Scanning..." : "Working...");
    portEXIT_CRITICAL(&mux);
    xSemaphoreGive(wake);
    return true;
}
