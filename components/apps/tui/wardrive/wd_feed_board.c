/* WARDRIVE's feed on the board (wd_feed.h).

   Wi-Fi: the same scan LINK runs (ls_wireless_request SCAN on the wireless
   worker), so an association in progress is kept: a station scan goes off
   channel between beacons and comes back, it never leaves the network. The
   full records (BSSID, security) are the scan cache ls_wifi keeps.
   Bluetooth: the find observer feeds ls_ble_heard; Remote ID: the RID
   observer feeds ls_rid. Other drone links come from DRONES' watch when it is
   running. Cells: CELL WATCH's LTE search, when the build has it and nothing
   else holds the receiver. */
#include "wd_feed.h"
#include "../ls_wireless.h"
#include "../../../../main/ls_wifi.h"
#include "../ls_ble_heard.h"
#include "../ls_rid.h"
#include "../ls_drone_watch.h"
#include "../ls_tui_screen.h"
#include "ls_gps.h"
#include "ls_imu.h"
#include "ls_location.h"
#include "ls_sdcard.h"
#include "ls_hub.h"
#include "ls_time.h"
#include "ls_task_reap.h"
#include "ls_board.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if !(defined(CONFIG_LS_LR2021_ONLY) && CONFIG_LS_LR2021_ONLY)
#define WD_HAS_CELL 1
#include "cell_monitor.h"
#else
#define WD_HAS_CELL 0
#endif

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define WD_DIR "/sdcard/wardrive"

static bool s_src[WD_KINDS], s_ble_on, s_rid_on, s_cell_on;
static int s_pace = WD_PACE_NORMAL;
static uint32_t s_wifi_req_ms, s_wifi_poll_ms, s_wifi_rev, s_ble_ms, s_rid_ms, s_dw_ms, s_cell_ms;
static bool s_wifi_rev_ok, s_wifi_pending;
static int64_t s_ble_seen_us, s_rid_seen_us;
static EXT_RAM_BSS_ATTR ls_wireless_snapshot_t s_snap;
static EXT_RAM_BSS_ATTR ls_wifi_scan_ap_t s_aps[64];
static EXT_RAM_BSS_ATTR ls_ble_heard_t s_ble[LS_BLE_HEARD_MAX];
static EXT_RAM_BSS_ATTR ls_rid_drone_t s_rid[LS_RID_MAX];
static EXT_RAM_BSS_ATTR ls_drone_watch_entry_t s_dw[LS_DW_ENTRIES];
static EXT_RAM_BSS_ATTR int64_t s_dw_seen[LS_DW_ENTRIES];
static EXT_RAM_BSS_ATTR ls_gps_state_t s_gps;
#if WD_HAS_CELL
static EXT_RAM_BSS_ATTR cell_status_t s_cell;
static EXT_RAM_BSS_ATTR cell_lte_t s_cells[CELL_MAX_CELLS];
#endif
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;

static uint8_t auth_of(unsigned a)
{
    switch (a) {
    case WIFI_AUTH_OPEN: return WD_AUTH_OPEN;
    case WIFI_AUTH_WEP: return WD_AUTH_WEP;
    case WIFI_AUTH_WPA_PSK: return WD_AUTH_WPA;
    case WIFI_AUTH_WPA2_PSK: return WD_AUTH_WPA2;
    case WIFI_AUTH_WPA_WPA2_PSK: return WD_AUTH_WPA_WPA2;
    case WIFI_AUTH_WPA2_ENTERPRISE: return WD_AUTH_WPA2_EAP;
    case WIFI_AUTH_WPA3_PSK: return WD_AUTH_WPA3;
    case WIFI_AUTH_WPA2_WPA3_PSK: return WD_AUTH_WPA2_WPA3;
    case WIFI_AUTH_WAPI_PSK: return WD_AUTH_WAPI;
    case WIFI_AUTH_OWE: return WD_AUTH_OWE;
    case WIFI_AUTH_WPA3_ENT_192: return WD_AUTH_WPA3_EAP;
    case WIFI_AUTH_WPA3_EXT_PSK: case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return WD_AUTH_WPA3;
    case WIFI_AUTH_WPA3_ENTERPRISE: case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return WD_AUTH_WPA3_EAP;
    case WIFI_AUTH_WPA_ENTERPRISE: return WD_AUTH_WPA2_EAP;
    default: return WD_AUTH_UNKNOWN;
    }
}

static uint32_t gap_ms(void) { return s_pace == WD_PACE_FAST ? 500 : s_pace == WD_PACE_SAVER ? 10000 : 2500; }

bool wd_feed_has(int kind)
{
    if (kind == WD_CELL) return WD_HAS_CELL;
    return kind >= 0 && kind < WD_KINDS;
}

void wd_feed_pace(int pace) { s_pace = pace; }

void wd_feed_start(const bool src[WD_KINDS], int pace)
{
    memcpy(s_src, src, sizeof(s_src));
    s_pace = pace;
    s_wifi_req_ms = s_wifi_poll_ms = s_ble_ms = s_rid_ms = s_dw_ms = s_cell_ms = 0;
    s_wifi_rev_ok = s_wifi_pending = false;
    s_ble_seen_us = s_rid_seen_us = esp_timer_get_time() - 2000000;
    memset(s_dw_seen, 0, sizeof(s_dw_seen));
    ls_location_want();
    if (src[WD_BLE]) { ls_wireless_find_ble(true); s_ble_on = true; }
    if (src[WD_DRONE]) { ls_wireless_rid_request(true); s_rid_on = true; }
    s_cell_on = false;
#if WD_HAS_CELL
    if (src[WD_CELL]) cell_monitor_init();      /* CELL WATCH's worker; once, PSRAM stack */
#endif
}

void wd_feed_stop(void)
{
    if (s_ble_on) ls_wireless_find_ble(false);
    if (s_rid_on) ls_wireless_rid_request(false);
#if WD_HAS_CELL
    if (s_cell_on) cell_monitor_stop();
#endif
    s_ble_on = s_rid_on = s_cell_on = false;
}

bool wd_feed_fix(wd_fix_t *f, uint32_t now)
{
    ls_gps_get(&s_gps);
    const int64_t us = esp_timer_get_time();
    f->fix = ls_gps_fix_current(&s_gps, us);
    f->lat = s_gps.lat_deg;
    f->lon = s_gps.lon_deg;
    f->alt_m = s_gps.alt_m;
    f->hdop = s_gps.hdop;
    f->sats = s_gps.sats_used;
    f->speed_mps = f->fix ? s_gps.speed_kts * 0.514444f : -1;
    f->course_deg = f->fix && s_gps.course_deg >= 0 && s_gps.course_deg <= 360 ? s_gps.course_deg : -1;
    const float h = ls_imu_heading();
    f->compass_deg = h >= 0 && h <= 360 ? h : -1;
    if (s_gps.utc_s > 1577836800u) f->utc = s_gps.utc_s + (uint32_t)((now - s_gps.utc_ms) / 1000u);
    else if (ls_time_is_synced()) f->utc = (uint32_t)time(NULL);
    else f->utc = 0;
    return f->fix;
}

static void wifi_poll(wd_obs_t *out, int cap, int *n, uint32_t now, unsigned *scans)
{
    if (now - s_wifi_poll_ms < 300) return;
    s_wifi_poll_ms = now;
    ls_wireless_get(&s_snap);
    if (!s_wifi_rev_ok) { s_wifi_rev = s_snap.scan_revision; s_wifi_rev_ok = true; }
    if (s_snap.scan_revision != s_wifi_rev) {
        int64_t seen = 0;
        const int k = ls_wifi_scan_cached(s_aps, 64, &seen);
        if (k < 0) return;                    /* the radio is busy: next time */
        s_wifi_rev = s_snap.scan_revision;
        s_wifi_pending = false;
        s_wifi_req_ms = now;
        (*scans)++;
        for (int i = 0; i < k && *n < cap; i++) {
            wd_obs_t *o = &out[(*n)++];
            memset(o, 0, sizeof(*o));
            o->kind = WD_WIFI;
            memcpy(o->addr, s_aps[i].bssid, 6);
            memcpy(o->name, s_aps[i].ssid, 32);
            o->name[32] = 0;
            o->auth = auth_of(s_aps[i].auth);
            o->channel = (uint8_t)s_aps[i].channel;
            o->rssi = (int8_t)(s_aps[i].rssi < -127 ? -127 : s_aps[i].rssi > 0 ? 0 : s_aps[i].rssi);
        }
        return;
    }
    if (s_snap.busy) return;
    /* A request that never ran (LINK had the worker) is asked again. */
    if (s_wifi_pending && now - s_wifi_req_ms < 15000) return;
    if (now - s_wifi_req_ms >= gap_ms() && ls_wireless_request(LS_WIRELESS_SCAN, NULL, NULL)) {
        s_wifi_pending = true;
        s_wifi_req_ms = now;
    }
}

static void ble_poll(wd_obs_t *out, int cap, int *n, uint32_t now)
{
    if (now - s_ble_ms < 1000) return;
    s_ble_ms = now;
    const int64_t us = esp_timer_get_time();
    const int k = ls_ble_heard_list(s_ble, LS_BLE_HEARD_MAX, us, 5000000);
    int64_t newest = s_ble_seen_us;
    for (int i = 0; i < k && *n < cap; i++) {
        if (s_ble[i].last_us <= s_ble_seen_us) continue;
        if (s_ble[i].last_us > newest) newest = s_ble[i].last_us;
        wd_obs_t *o = &out[(*n)++];
        memset(o, 0, sizeof(*o));
        o->kind = WD_BLE;
        memcpy(o->addr, s_ble[i].addr, 6);
        snprintf(o->name, sizeof(o->name), "%.*s", (int)sizeof(s_ble[i].name), s_ble[i].name);
        o->rssi = s_ble[i].rssi;
    }
    s_ble_seen_us = newest;
}

static void drone_poll(wd_obs_t *out, int cap, int *n, uint32_t now)
{
    const int64_t us = esp_timer_get_time();
    if (now - s_rid_ms >= 1000) {
        s_rid_ms = now;
        if (s_rid_on && ls_wireless_rid_status() == LS_RID_RX_OFF) ls_wireless_rid_request(true);
        const size_t k = ls_rid_snapshot(s_rid, LS_RID_MAX, us);
        int64_t newest = s_rid_seen_us;
        for (size_t i = 0; i < k && i < LS_RID_MAX && *n < cap; i++) {
            if (s_rid[i].last_us <= s_rid_seen_us) continue;
            if (s_rid[i].last_us > newest) newest = s_rid[i].last_us;
            wd_obs_t *o = &out[(*n)++];
            memset(o, 0, sizeof(*o));
            o->kind = WD_DRONE;
            memcpy(o->addr, s_rid[i].mac, 6);
            snprintf(o->name, sizeof(o->name), "%s", s_rid[i].uas_id[0] ? s_rid[i].uas_id : "REMOTE ID");
            o->rssi = s_rid[i].rssi;
            if (s_rid[i].position_valid && isfinite(s_rid[i].lat) && isfinite(s_rid[i].lon) &&
                (s_rid[i].lat != 0 || s_rid[i].lon != 0)) {
                o->own_pos = true; o->lat = s_rid[i].lat; o->lon = s_rid[i].lon;
            }
        }
        s_rid_seen_us = newest;
    }
    if (now - s_dw_ms >= 2000) {              /* DRONES' watch, when it is listening */
        s_dw_ms = now;
        const size_t k = ls_drone_watch_snapshot(s_dw, LS_DW_ENTRIES);
        if (k == SIZE_MAX) return;
        for (size_t i = 0; i < k && i < LS_DW_ENTRIES && *n < cap; i++) {
            const ls_drone_watch_entry_t *e = &s_dw[i];
            if (e->source == LS_DW_RID || e->last_us <= s_dw_seen[i]) continue;
            s_dw_seen[i] = e->last_us;
            wd_obs_t *o = &out[(*n)++];
            memset(o, 0, sizeof(*o));
            o->kind = WD_DRONE;
            for (int b = 0; b < 6; b++) o->addr[b] = (uint8_t)(e->id >> (8 * (5 - b)));
            snprintf(o->name, sizeof(o->name), "%s %s", ls_drone_watch_label(e->source), e->channel);
            const float d = e->rssi_dbm < -127 ? -127 : e->rssi_dbm > 0 ? 0 : e->rssi_dbm;
            o->rssi = (int8_t)d;
        }
    }
}

#if WD_HAS_CELL
static void cell_poll(wd_obs_t *out, int cap, int *n, uint32_t now)
{
    if (now - s_cell_ms < 3000) return;
    s_cell_ms = now;
    cell_monitor_get(&s_cell);
    /* Keep the LTE search going while nothing else wants the receiver. */
    if (!s_cell.busy && !s_cell.receiver_missing && !ls_tui_radio_claimed())
        s_cell_on = cell_monitor_request(CELL_LTE, 0, false) || s_cell_on;
    const unsigned k = cell_monitor_cells(s_cells, CELL_MAX_CELLS);
    for (unsigned i = 0; i < k && *n < cap; i++) {
        const cell_lte_t *c = &s_cells[i];
        wd_obs_t *o = &out[(*n)++];
        memset(o, 0, sizeof(*o));
        o->kind = WD_CELL;
        o->addr[2] = (uint8_t)(c->earfcn >> 8); o->addr[3] = (uint8_t)c->earfcn;
        o->addr[4] = (uint8_t)(c->pci >> 8);    o->addr[5] = (uint8_t)c->pci;
        snprintf(o->name, sizeof(o->name), "LTE %u PCI %u", (unsigned)c->earfcn, (unsigned)c->pci);
        const int p = c->rsrp10 == CELL_NO_POWER ? -110 : c->rsrp10 / 10;
        o->rssi = (int8_t)(p < -127 ? -127 : p > 0 ? 0 : p);
    }
}
#endif

int wd_feed_poll(wd_obs_t *out, int cap, uint32_t now, unsigned *scans)
{
    int n = 0;
    if (s_src[WD_WIFI]) wifi_poll(out, cap, &n, now, scans);
    if (s_src[WD_BLE]) ble_poll(out, cap, &n, now);
    if (s_src[WD_DRONE]) drone_poll(out, cap, &n, now);
#if WD_HAS_CELL
    if (s_src[WD_CELL]) cell_poll(out, cap, &n, now);
#endif
    return n;
}

/* ------------------------------------------------------------ the worker */

static void worker(void *arg)
{
    (void)arg;
    do {
        wd_step((uint32_t)(esp_timer_get_time() / 1000));
        vTaskDelay(pdMS_TO_TICKS(250));
    } while (wd_busy());
    s_task = NULL;
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    ls_task_retire_self();      /* no helper task in internal RAM (ls_task_reap.h) */
#else
    vTaskDelete(NULL);
#endif
}

bool wd_feed_spawn(void)
{
    if (s_task) return true;
    /* Scans, notes and the card: no NVS or internal flash, so the stack may
       be PSRAM, as the track recorder's is. */
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    const BaseType_t ok = xTaskCreateWithCaps(worker, "wardrive", 6144, NULL, 3, &s_task,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    const BaseType_t ok = xTaskCreate(worker, "wardrive", 6144, NULL, 3, &s_task);
#endif
    if (ok != pdPASS) s_task = NULL;
    return s_task != NULL;
}

bool wd_feed_worker_alive(void) { return s_task != NULL; }

void wd_lock(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}
void wd_unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

void *wd_feed_alloc(size_t bytes) { return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void wd_feed_free(void *p) { heap_caps_free(p); }

bool wd_feed_card(void) { return ls_sdcard_mounted(); }

const char *wd_feed_dir(void)
{
    struct stat st;
    if (stat(WD_DIR, &st) == 0) return WD_DIR;
    if (mkdir(WD_DIR, 0775) == 0 || errno == EEXIST) return WD_DIR;
    return "";
}

const char *wd_feed_board(void)
{
#ifdef LS_BOARD_NAME
    return LS_BOARD_NAME;
#else
    return "LakeShark";
#endif
}

bool wd_feed_upload(const char *path) { return ls_locker_upload(path); }

bool wd_feed_hub(char *out, size_t cap, bool *busy)
{
    ls_hub_status_t st;
    ls_hub_status(&st);
    snprintf(out, cap, "%s", st.text);
    *busy = st.state == LS_HUB_BUSY;
    return true;
}
