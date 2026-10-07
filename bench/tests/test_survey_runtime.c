#include "ls_test.h"
#include "ls_survey.h"
#include "ls_wifi.h"
#include "ble_link.h"
#include "ls_gps.h"
#include <dirent.h>
#include <unistd.h>

extern void ls_shim_time_set(int64_t us);
extern bool ls_survey_pending(void);
extern void ls_survey_tick(void);
static bool wifi_busy, wifi_owned, ble_owned;
static int wifi_scans, ble_starts;
static ls_gps_state_t gps;
static ls_survey_options_t options = {true, true, false, 3, 1};
void ls_wireless_set_active(bool active) { (void)active; }
bool ls_wifi_sta_running(void) { return wifi_busy; }
bool ls_wifi_running(void) { return false; }
esp_err_t ls_wifi_survey_mode(bool active) { wifi_owned = active; return ESP_OK; }
ble_link_state_t ble_link_state(void) { return ble_owned ? BLE_LINK_SCANNING : BLE_LINK_OFF; }
esp_err_t ble_link_listen(void) { ble_owned = true; ble_starts++; return ESP_OK; }
void ble_link_stop(void) { ble_owned = false; }
bool ls_gps_running(void) { return true; }
esp_err_t ls_gps_start(void) { return ESP_OK; }
void ls_gps_get(ls_gps_state_t *out) { *out = gps; }
int ls_wifi_survey_scan(ls_wifi_scan_ap_t *out, int cap)
{
    LS_CHECK(wifi_owned); LS_CHECK(cap >= 1);
    wifi_scans++;
    *out = (ls_wifi_scan_ap_t){.rssi=-60, .channel=6, .auth=3};
    out->bssid[5] = 1; strcpy(out->ssid, "Cafe, \"north\"");
    return 1;
}
static void clean(void)
{
    DIR *d = opendir(LS_SURVEY_DIRECTORY);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[512]; snprintf(path, sizeof(path), "%s/%s", LS_SURVEY_DIRECTORY, e->d_name);
        unlink(path);
    }
    closedir(d);
}

LS_CASE(runtime_is_periodic_exports_retains_and_rejects_busy_radios)
{
    clean();
    ls_shim_time_set(1000000);
    gps = (ls_gps_state_t){.fix=true, .lat_deg=42, .lon_deg=-71,
        .last_fix_us=1000000, .last_sentence_us=1000000,
        .year=2026, .month=10, .day=7, .hour=12};
    wifi_busy = true;
    LS_CHECK(ls_survey_run(true, &options)); ls_survey_tick();
    ls_survey_view_t v; ls_survey_view(&v);
    LS_CHECK(!v.running); LS_EQ_INT(ble_starts, 0); LS_EQ_INT(wifi_scans, 0);
    wifi_busy = false;
    LS_CHECK(ls_survey_run(true, &options)); ls_survey_tick();
    ls_survey_view(&v); LS_CHECK(v.running); LS_CHECK(ls_survey_pending());
    LS_CHECK(!ls_survey_run(true, &options));
    LS_EQ_INT(wifi_scans, 1); LS_EQ_INT(ble_starts, 1);
    ls_shim_time_set(2000000); ls_survey_tick(); LS_EQ_INT(wifi_scans, 1);
    uint8_t addr[6] = {1,2,3,4,5,6};
    ls_survey_ble(addr, 1, "Tag", 3, -30, 2000000);
    ls_survey_view(&v); LS_EQ_UINT(v.wifi, 1); LS_EQ_UINT(v.ble, 1);
    ls_survey_entry_t e;
    LS_CHECK(ls_survey_at(0, 0, &e)); LS_EQ_INT(e.rssi, -30);
    LS_EQ_UINT(e.addr[0], 6); LS_CHECK(e.positioned);
    ls_shim_time_set(4000000); ls_survey_tick(); LS_EQ_INT(wifi_scans, 2);
    LS_CHECK(ls_survey_run(false, NULL));
    ls_survey_ble(addr, 1, "Tag", 3, -10, 4000000);
    LS_CHECK(ls_survey_at(0, 0, &e)); LS_EQ_INT(e.rssi, -30);
    ls_survey_tick(); ls_survey_view(&v);
    LS_CHECK(!v.running); LS_CHECK(!wifi_owned && !ble_owned);
    LS_CHECK(!ls_survey_pending()); LS_CHECK(strstr(v.path, "20261007_120000.csv"));
    FILE *f = fopen(v.path, "r"); LS_CHECK(f != NULL);
    char csv[2048] = "";
    if (f) { fread(csv, 1, sizeof(csv)-1, f); fclose(f); }
    LS_CHECK(strstr(csv, "Cafe, \"\"north\"\""));
    LS_CHECK(strstr(csv, "ble,\"Tag\",06:05:04:03:02:01,1,,,-30,"));
    LS_CHECK(strstr(csv, "42.0000000,-71.0000000"));
    char first[128]; strcpy(first, v.path);
    char other[256]; snprintf(other, sizeof(other), "%s/walk-notes.txt", LS_SURVEY_DIRECTORY);
    f = fopen(other, "w"); LS_CHECK(f != NULL); if (f) fclose(f);
    /* Starting twice in the same GPS second must not overwrite the export. */
    LS_CHECK(ls_survey_run(true, &options)); ls_survey_tick();
    LS_CHECK(ls_survey_run(false, NULL)); ls_survey_tick(); ls_survey_view(&v);
    LS_CHECK(strstr(v.path, "20261007_120000_001.csv"));
    f = fopen(first, "r"); LS_CHECK(!f); if (f) fclose(f);
    f = fopen(v.path, "r"); LS_CHECK(f != NULL); if (f) fclose(f);
    f = fopen(other, "r"); LS_CHECK(f != NULL); if (f) fclose(f);
    /* Advert callbacks after stop cannot mutate the retained summary. */
    unsigned count = v.wifi + v.ble;
    ls_survey_ble(addr, 1, "Tag", 3, -10, 4000000);
    ls_survey_view(&v); LS_EQ_UINT(v.wifi + v.ble, count);
    clean();
}
