#include "ls_test.h"
#include "ls_survey.h"
#include "ls_wifi.h"
#include "ble_link.h"
#include "ls_gps.h"
#include "ls_version.h"
#include <dirent.h>
#include <unistd.h>

extern void ls_shim_time_set(int64_t us);
extern bool ls_survey_pending(void);
extern void ls_survey_tick(void);
void ls_version_get(ls_version_info_t *v) { *v=(ls_version_info_t){.version="2.13.2",.board="bench"}; }
bool ls_sdcard_size(uint64_t *total, uint64_t *free_bytes) { *total=1024*1024; *free_bytes=512*1024; return true; }
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
    clean(); wifi_scans=ble_starts=0;
    ls_shim_time_set(1000000);
    gps = (ls_gps_state_t){.fix=true, .lat_deg=42, .lon_deg=-71,
        .last_fix_us=1000000, .last_sentence_us=1000000,
        .year=2026, .month=10, .day=7, .hour=12, .utc_s=1791374400, .utc_ms=1000};
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
    LS_CHECK(strstr(csv, "06:05:04:03:02:01,\"Tag\",[BLE],2026-10-07 12:00:00,0,0,-30,"));
    LS_CHECK(strstr(csv, "42.0000000,-71.0000000"));
    char first[128]; strcpy(first, v.path);
    char other[256]; snprintf(other, sizeof(other), "%s/walk-notes.txt", LS_SURVEY_DIRECTORY);
    f = fopen(other, "w"); LS_CHECK(f != NULL); if (f) fclose(f);
    /* Starting twice in the same GPS second must not overwrite the export. */
    gps.utc_ms=4000;
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

LS_CASE(stream_survives_cache_eviction_and_flushes_before_stop)
{
    clean(); ls_shim_time_set(1000000);
    gps=(ls_gps_state_t){.fix=true,.lat_deg=42,.lon_deg=-71,.last_fix_us=1000000,
        .utc_s=1791374400,.utc_ms=1000,.alt_m=100,.hdop=2};
    ls_survey_options_t o={false,true,true,3,2};
    LS_CHECK(ls_survey_run(true,&o)); ls_survey_tick();
    for(unsigned i=0;i<LS_SURVEY_CAP+10;i++) {
        uint8_t addr[6]={(uint8_t)i,(uint8_t)(i>>8),0,0,0,0};
        ls_survey_ble(addr,0,"test",4,-70,1000000);
    }
    ls_survey_view_t v; ls_survey_view(&v);
    LS_EQ_UINT(v.rows,LS_SURVEY_CAP+10); LS_EQ_UINT(v.cached,LS_SURVEY_CAP);
    LS_EQ_UINT(v.ble,LS_SURVEY_CAP+10); LS_EQ_UINT(v.dropped,10);
    ls_shim_time_set(6000000); ls_survey_tick(); ls_survey_view(&v);
    LS_CHECK(v.sd_known); LS_EQ_UINT(v.free_bytes,512*1024);
    LS_EQ_UINT(v.ble_minute,LS_SURVEY_CAP+10);
    ls_shim_time_set(62000000); ls_survey_tick(); ls_survey_view(&v);
    LS_EQ_UINT(v.ble_minute,0); LS_EQ_UINT(v.ble,LS_SURVEY_CAP+10);
    FILE *f=fopen(v.path,"r"); LS_CHECK(f); unsigned lines=0; int c;
    if(f) { while((c=fgetc(f))!=EOF) if(c=='\n') lines++; fclose(f); }
    LS_EQ_UINT(lines,LS_SURVEY_CAP+12);
    LS_CHECK(ls_survey_run(false,NULL)); ls_survey_tick();
    char path[160]; snprintf(path,sizeof(path),"%.*s_summary.csv",(int)strlen(v.path)-4,v.path);
    f=fopen(path,"r"); LS_CHECK(f); lines=0;
    if(f) { while((c=fgetc(f))!=EOF) if(c=='\n') lines++; fclose(f); }
    LS_EQ_UINT(lines,LS_SURVEY_CAP+11); clean();
}

LS_CASE(no_utc_no_file_then_acquire_lose_and_recover)
{
    clean(); ls_shim_time_set(1000000); gps=(ls_gps_state_t){0};
    ls_survey_options_t o={false,true,false,3,2};
    uint8_t addr[6]={1,2,3,4,5,6};
    LS_CHECK(ls_survey_run(true,&o)); ls_survey_tick();
    ls_survey_ble(addr,0,"tag",3,-70,1000000);
    ls_survey_view_t v; ls_survey_view(&v); LS_EQ_UINT(v.rows,0); LS_CHECK(!v.path[0]);
    gps=(ls_gps_state_t){.fix=true,.lat_deg=42,.lon_deg=-71,.last_fix_us=2000000,
        .utc_s=1791374400,.utc_ms=2000};
    ls_shim_time_set(2000000); ls_survey_tick();
    ls_survey_ble(addr,0,"tag",3,-70,2000000);
    ls_survey_view(&v); LS_EQ_UINT(v.rows,1); LS_CHECK(v.utc_valid);
    ls_shim_time_set(6000000); ls_survey_tick();
    ls_survey_ble(addr,0,"tag",3,-60,6000000);
    ls_survey_view(&v); LS_EQ_UINT(v.rows,1); LS_CHECK(!v.utc_valid);
    gps.utc_ms=6000; gps.last_fix_us=6000000; ls_survey_tick();
    ls_survey_ble(addr,0,"tag",3,-60,6000000);
    ls_survey_view(&v); LS_EQ_UINT(v.rows,2);
    gps.lat_deg+=0.001; ls_survey_tick();
    ls_survey_ble(addr,0,"tag",3,-80,6000000);
    ls_survey_view(&v); LS_EQ_UINT(v.rows,3);
    LS_CHECK(ls_survey_run(false,NULL)); ls_survey_tick(); clean();
}

LS_CASE(sd_open_error_is_visible_and_does_not_claim_rows_saved)
{
    clean(); rmdir(LS_SURVEY_DIRECTORY);
    FILE *blocker=fopen(LS_SURVEY_DIRECTORY,"w"); LS_CHECK(blocker); if(!blocker) return;
    fclose(blocker);
    ls_shim_time_set(1000000);
    gps=(ls_gps_state_t){.utc_s=1791374400,.utc_ms=1000};
    ls_survey_options_t o={false,true,false,3,2};
    LS_CHECK(ls_survey_run(true,&o)); ls_survey_tick();
    ls_survey_view_t v; ls_survey_view(&v);
    LS_CHECK(strstr(v.status,"SD open failed")); LS_EQ_UINT(v.rows,0);
    LS_CHECK(ls_survey_run(false,NULL)); ls_survey_tick(); ls_survey_view(&v);
    LS_CHECK(strstr(v.status,"SD export failed"));
    unlink(LS_SURVEY_DIRECTORY);
}
