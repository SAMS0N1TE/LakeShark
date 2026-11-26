/* WARDRIVE's feed on the board (wd_feed.h).

   Wi-Fi: the same scan LINK runs (ls_wireless_request SCAN on the wireless
   worker), so an association in progress is kept: a station scan goes off
   channel between beacons and comes back, it never leaves the network. The
   full records (BSSID, security) are the scan cache ls_wifi keeps.
   Bluetooth: the find observer feeds ls_ble_heard; Remote ID: the RID
   observer feeds ls_rid. Other drone links come from DRONES' watch when it is
   running. Cells: CELL WATCH's LTE search, when the build has it and nothing
   else holds the receiver. The LoRa chip and a USB SDR run WARDRIVE's radio
   jobs (wd_radio.h); their port is at the end of this file. */
#include "wd_feed.h"
#include "wd_radio.h"
#include "../ls_wireless.h"
#include "../../../../main/ls_wifi.h"
#include "../ls_ble_heard.h"
#include "../ls_ble_names.h"
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

/* ------------------------------------------------------------ PARTY */

/* Members' finds, when a party runs: PARTY's party_peer_since, reached by
   name (both apps are modules), every three seconds while a drive runs.
   Before PARTY has been opened the call is not there and nothing comes. */
#include "../party/party_wd_peer.h"
#if defined(CONFIG_LS_APP_MODULES) && CONFIG_LS_APP_MODULES
#include "../ls_mod.h"
#endif
static EXT_RAM_BSS_ATTR party_find_t s_party_finds[16];
static uint32_t s_party_ms, s_party_since;

static int party_poll(wd_obs_t *out, int cap, uint32_t now)
{
    if (now - s_party_ms < 3000 || cap <= 0) return 0;
    s_party_ms = now;
#if defined(CONFIG_LS_APP_MODULES) && CONFIG_LS_APP_MODULES
    int (*since)(party_find_t *, int, uint32_t) =
        (int (*)(party_find_t *, int, uint32_t))ls_mod_sym("party", "party_peer_since", false);
#else
    int (*since)(party_find_t *, int, uint32_t) = party_peer_since;
#endif
    if (!since) return 0;
    if (!s_party_since) s_party_since = now - 60000;   /* the last minute's when a drive starts */
    const int n = since(s_party_finds, 16, s_party_since);
    for (int i = 0; i < n; i++)
        if ((int32_t)(s_party_finds[i].ms - s_party_since) > 0) s_party_since = s_party_finds[i].ms;
    return party_peers_to_obs(s_party_finds, n, out, cap);
}

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
    if (kind == WD_RF || kind == WD_AIR || kind == WD_LORA) return wd_radio_has(kind);
    return kind >= 0 && kind < WD_KINDS;
}

void wd_feed_pace(int pace) { s_pace = pace; }

void wd_feed_start(const bool src[WD_KINDS], int pace)
{
    memcpy(s_src, src, sizeof(s_src));
    s_pace = pace;
    s_wifi_req_ms = s_wifi_poll_ms = s_ble_ms = s_rid_ms = s_dw_ms = s_cell_ms = 0;
    s_wifi_rev_ok = s_wifi_pending = false;
    s_party_ms = s_party_since = 0;
    s_ble_seen_us = s_rid_seen_us = esp_timer_get_time() - 2000000;
    memset(s_dw_seen, 0, sizeof(s_dw_seen));
    ls_location_want();
    if (src[WD_BLE]) { ls_wireless_find_ble(true); s_ble_on = true; }
    if (src[WD_DRONE]) { ls_wireless_rid_request(true); s_rid_on = true; }
    s_cell_on = false;
#if WD_HAS_CELL
    if (src[WD_CELL]) cell_monitor_init();      /* CELL WATCH's worker; once, PSRAM stack */
#endif
    wd_radio_start(src);
}

void wd_feed_stop(void)
{
    if (s_ble_on) ls_wireless_find_ble(false);
    if (s_rid_on) ls_wireless_rid_request(false);
#if WD_HAS_CELL
    if (s_cell_on) cell_monitor_stop();
#endif
    s_ble_on = s_rid_on = s_cell_on = false;
    wd_radio_stop();
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
        /* wd_obs_t keeps addresses as shown (the WiGLE MAC, the signature
           table's prefixes); the radio hears them least significant first */
        for (int b = 0; b < 6; b++) o->addr[b] = s_ble[i].addr[5 - b];
        snprintf(o->name, sizeof(o->name), "%.*s", (int)sizeof(s_ble[i].name), s_ble[i].name);
        /* a name heard is remembered for the board and every BLE app
           (ble-devices.txt); a device advertising without one gets the
           name it gave before, if it gave one */
        if (o->name[0]) ls_ble_names_learn(o->addr, s_ble[i].addr_type, o->name);
        else ls_ble_names_device(o->addr, s_ble[i].addr_type, o->name, sizeof(o->name));
        o->ad_have = s_ble[i].ad_have;
        o->company = s_ble[i].company; o->mfg0 = s_ble[i].mfg0;
        o->uuid16 = s_ble[i].uuid16; o->svc0 = s_ble[i].svc0;
        o->appearance = s_ble[i].appearance;
        if (s_ble[i].addr_type != LS_BLE_HEARD_TYPE_UNKNOWN) o->ble_type = (uint8_t)(s_ble[i].addr_type + 1);
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
    if ((s_src[WD_RF] || s_src[WD_AIR] || s_src[WD_LORA]) && n < cap) n += wd_radio_poll(out + n, cap - n, now);
    if (n < cap) n += party_poll(out + n, cap - n, now);
    return n;
}

/* ------------------------------------------------------------ the worker */

static void lora_service(void);
static bool lora_idle(void);

static void worker(void *arg)
{
    (void)arg;
    do {
        wd_step((uint32_t)(esp_timer_get_time() / 1000));
        /* The LoRa chip's session is read ten times between passes: its
           part holds one frame, and a tyre sensor's burst is short. The
           lease is this task's, so it is let go here too. */
        for (int i = 0; i < 10; i++) {
            lora_service();
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    } while (wd_busy() || !lora_idle());
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

/* ======================================================= the radio port

   wd_radio.h's port on the board. Two radios:

   The LoRa chip runs its jobs as a background service would: the lease is
   ls_mesh_radio_hold_svc, below every radio app, MeshCore's screen and its
   sends, so whoever else asks gets the chip and the job waits. TPMS and ISM
   are rtl_433's decoders (lr433_dec.c, in the firmware) on the chip's OOK and
   FSK sessions run as samplers, the sessions LR433 runs; MESH listens with
   Meshtastic's and MeshCore's settings in turn and reads the headers. While
   MeshCore runs, MESH is MeshCore itself: the chip is left to it and its
   heard adverts are read.

   The SDR runs a receiver mode (ADS-B, or REC with rec_ism_listen stepping
   the TPMS/ISM frequencies), asked for only while no app has one, and
   started or switched only while WARDRIVE is on screen: a receiver another
   app may be using is never retuned. ADS-B goes on in the background.

   What other apps' receivers hear comes in for free: rtl_433 decodes from
   SUB-GHZ WATCH, METERS and REC (rec_ism's sink), the ADS-B table, the
   MESHTASTIC app's nodes (ls_extnodes) and MeshCore's adverts. */

#include "ls_lora.h"
#include "ls_mesh.h"
#include "rec_ism.h"
#include "rec_watch.h"
#include "apps/adsb/adsb_state.h"
#include "apps/adsb/adsb_app.h"
#include "../ls_extnodes.h"
#include "../ls_field.h"
#include "../ls_radio_select.h"
#include "../experiments/lr433_plan.h"

enum { S_OOK, S_FSK, S_LORA };
typedef struct {
    uint8_t kind;
    uint32_t hz;
    /* lr433 sampler: decisions a second, filter, deviation, sync/pattern,
       what goes back in front, frame bytes */
    uint32_t rate, bw, dev;
    uint16_t sync;
    uint8_t sync_bits;
    uint32_t lead;
    uint8_t lead_bits, frame;
    /* LoRa */
    uint8_t sf, cr, word, flavour;
} sess_t;

/* LR433's sessions (experiments/lr433_plan.h, shared with LR433), by job and region. */
#define OOK_TPMS(f)  { S_OOK, f, LR433_P_OOK_TPMS, 0, 0, 0, 0 }
#define FSK_TPMS(f)  { S_FSK, f, LR433_P_FSK_TPMS, 0, 0, 0, 0 }
#define FSK_PMV(f)   { S_FSK, f, LR433_P_FSK_PMV, 0, 0, 0, 0 }
#define OOK_WX(f)    { S_OOK, f, LR433_P_OOK_WX, 0, 0, 0, 0 }
#define FSK_WX(f)    { S_FSK, f, LR433_P_FSK_WX, 0, 0, 0, 0 }
#define LORA_MT(f)   { S_LORA, f, 0, 250000, 0, 0, 0, 0, 0, 0, 11, 5, 0x2B, WD_MESH_MESHTASTIC }
#define LORA_MC(f, bw, sf, cr) { S_LORA, f, 0, bw, 0, 0, 0, 0, 0, 0, sf, cr, 0x12, WD_MESH_MESHCORE }

static const sess_t S_TPMS_NA[] = { OOK_TPMS(315000000u), FSK_TPMS(315000000u), FSK_PMV(315000000u) };
static const sess_t S_TPMS_EU[] = { FSK_TPMS(433920000u), OOK_TPMS(433920000u) };
static const sess_t S_ISM_NA[]  = { OOK_WX(433920000u), FSK_WX(915000000u) };
static const sess_t S_ISM_EU[]  = { OOK_WX(433920000u), FSK_WX(868300000u) };
static const sess_t S_MESH_NA[] = { LORA_MT(906875000u), LORA_MC(910525000u, 62500, 7, 5) };
static const sess_t S_MESH_EU[] = { LORA_MT(869525000u), LORA_MC(869618000u, 62500, 8, 8) };
static const sess_t S_MESH_AU[] = { LORA_MT(919875000u), LORA_MC(915800000u, 250000, 10, 5) };
#define NS(a) ((int)(sizeof(a) / sizeof(a[0])))
#define SESS_US 12000000LL          /* each session of a job, in turn */

static void sessions(int job, int region, const sess_t **out, int *n)
{
    *out = NULL; *n = 0;
    switch (job) {
    case WD_JOB_TPMS:
        if (region == WD_REGION_NA) { *out = S_TPMS_NA; *n = NS(S_TPMS_NA); }
        else { *out = S_TPMS_EU; *n = NS(S_TPMS_EU); }
        break;
    case WD_JOB_ISM:
        if (region == WD_REGION_EU) { *out = S_ISM_EU; *n = NS(S_ISM_EU); }
        else { *out = S_ISM_NA; *n = NS(S_ISM_NA); }
        break;
    case WD_JOB_MESH:
        if (region == WD_REGION_EU) { *out = S_MESH_EU; *n = NS(S_MESH_EU); }
        else if (region == WD_REGION_AU) { *out = S_MESH_AU; *n = NS(S_MESH_AU); }
        else { *out = S_MESH_NA; *n = NS(S_MESH_NA); }
        break;
    default: break;
    }
}

static bool sess_ok(const sess_t *s, uint32_t caps)
{
    if (s->kind == S_LORA) return (caps & LS_LORA_CAP_LORA) != 0;
    if (s->kind == S_FSK) return (caps & LS_LORA_CAP_FSK) != 0;
    return (caps & LS_LORA_CAP_MODES_RX) != 0;
}

/* ---- what the radios are (cached: ls_rsel_hw keeps static buffers) ---- */

static struct {
    int64_t at_us;
    bool lora, rtl, hackrf;
    uint32_t caps;
    char lora_name[12];
    int adsb;                       /* ls_rsel_resolved(LS_RSEL_ADSB) */
} s_hw;

static void hw_refresh(void)
{
    const int64_t us = esp_timer_get_time();
    if (s_hw.at_us && us - s_hw.at_us < 2000000) return;
    static EXT_RAM_BSS_ATTR ls_rsel_hw_t hw;
    ls_rsel_hw(&hw);
    s_hw.lora = hw.present[LS_RSEL_LORA];
    s_hw.rtl = hw.present[LS_RSEL_SDR_RTL];
    s_hw.hackrf = hw.present[LS_RSEL_SDR_HACKRF];
    s_hw.caps = hw.lora_caps;
    snprintf(s_hw.lora_name, sizeof(s_hw.lora_name), "%s", ls_rsel_name(LS_RSEL_LORA));
    s_hw.adsb = ls_rsel_resolved(LS_RSEL_ADSB);
    s_hw.at_us = us;
}

/* ---- the receiver mode the SDR jobs use (one at a time, the board's) ---- */

static struct {
    const char *mode;               /* the mode we asked for, NULL none */
    int screen;                     /* the screen shown when we asked */
    int user;                       /* the radio whose job it is */
    bool listen;                    /* rec_ism_listen is stepping our plan */
    int job, region;
    uint32_t plan[4];
    int plan_n;
    char busy[28];
    bool waiting;
} M = { .user = -1 };

static bool on_wardrive(void)
{
    const char *n = ls_tui_screen_name(ls_tui_screen_current());
    return n && !strcmp(n, "WARDRIVE");
}
/* Retune or switch the receiver: only while WARDRIVE is shown, or the screen
   we asked from still is. */
static bool may_switch(void) { return on_wardrive() || ls_tui_screen_current() == M.screen; }

static void listen_off(void)
{
    if (!M.listen) return;
    rec_ism_listen_t st;
    rec_ism_listen_status(&st);
    bool ours = false;
    for (int i = 0; i < M.plan_n; i++) ours |= st.hz == M.plan[i];
    if (st.on && ours) rec_ism_listen(NULL, 0, 0);
    M.listen = false;
}

static void mode_release(void)
{
    listen_off();
    const char *c = ls_tui_radio_claimed();
    if (M.mode && c && !strcmp(c, M.mode) && may_switch()) ls_tui_radio_want(NULL);
    M.mode = NULL;
    M.user = -1;
    M.waiting = false;
    M.busy[0] = 0;
}

/* Keep radio r's job's mode going. */
static void mode_keep(int r)
{
    const char *want = M.job == WD_JOB_AIR ? "ADS-B" : "REC";
    const char *c = ls_tui_radio_claimed();
    M.busy[0] = 0;
    M.waiting = false;
    if (c && (!M.mode || strcmp(c, M.mode))) {
        /* An app has the receiver: hands off. */
        if (M.mode) { M.listen = false; M.mode = NULL; }
        snprintf(M.busy, sizeof(M.busy), "%s", c);
        return;
    }
    if (!c || strcmp(M.mode, want)) {
        if (!may_switch()) { M.waiting = true; return; }   /* open WARDRIVE to start it */
        listen_off();
        ls_tui_radio_want(want);
        M.mode = want;
        M.screen = ls_tui_screen_current();
        M.user = r;
        M.waiting = true;
        return;
    }
    M.user = r;
    if (M.job == WD_JOB_AIR) return;            /* the ADS-B table is read with the rest */
    if (!M.listen) {
        if (!may_switch()) { M.waiting = true; return; }
        uint32_t hz[4];
        for (int i = 0; i < M.plan_n; i++) hz[i] = M.plan[i];
        if (rec_ism_listen(hz, M.plan_n, 15000)) M.listen = true;
        else { M.waiting = true; snprintf(M.busy, sizeof(M.busy), "SUB-GHZ WATCH"); }
    } else if (!may_switch()) {
        listen_off();                           /* another screen may be using REC */
        M.waiting = true;
    }
}

/* ---- the LoRa chip: its own task's lease, serviced by the worker ---- */

static struct {
    volatile int job, region;       /* wanted (set from any task) */
    int run_job, run_region;        /* what the sessions are for */
    const sess_t *s;
    int n, i;
    bool asked, open;
    int64_t sess_end;
    char busy[28];
    bool waiting, mc;               /* MeshCore is doing MESH */
    uint32_t khz;

    volatile int head, tail;
} L = { .job = -1, .run_job = -1 };
static EXT_RAM_BSS_ATTR wd_obs_t s_ring[32];   /* what the sessions heard, for the next poll */

static EXT_RAM_BSS_ATTR lr433_work_t s_lr_work;
static EXT_RAM_BSS_ATTR lr433_msg_t s_lr_msgs[4];
static EXT_RAM_BSS_ATTR uint8_t s_lr_buf[256];

static void ring_put(const wd_obs_t *o)
{
    const int nx = (L.head + 1) % (int)(sizeof(s_ring) / sizeof(s_ring[0]));
    if (nx == L.tail) return;
    s_ring[L.head] = *o;
    L.head = nx;
}

static void sess_close(void)
{
    if (!L.open) return;
    const sess_t *s = &L.s[L.i];
    if (s->kind == S_FSK) ls_lora_fsk_end();
    else if (s->kind == S_OOK) ls_lora_ook_end();
    else ls_lora_park();
    L.open = false;
}

static void lora_close(void)
{
    sess_close();
    if (L.asked) { ls_mesh_radio_hold_svc(false); L.asked = false; }
    L.khz = 0;
}

static esp_err_t sess_begin(const sess_t *s)
{
    if (s->kind == S_FSK) {
        const ls_fsk_cfg_t c = { .freq_hz = s->hz, .bitrate = s->rate, .deviation_hz = s->dev,
                                 .bandwidth_hz = ls_lora_fsk_bw_snap(s->bw),
                                 .sync_word = (uint32_t)s->sync << (32 - s->sync_bits),
                                 .payload_bytes = s->frame, .sync_bits = s->sync_bits };
        return ls_lora_fsk_begin(&c);
    }
    if (s->kind == S_OOK) {
        const ls_ook_cfg_t c = { .freq_hz = s->hz, .bitrate = s->rate, .bandwidth_hz = s->bw,
                                 .pattern = s->sync, .pattern_bits = s->sync_bits, .frame_bytes = s->frame,
                                 .gain_step = 13, .boost = -1 };
        return ls_lora_ook_begin(&c);
    }
    ls_lora_cfg_t c;
    ls_lora_cfg_default(&c);
    c.freq_hz = s->hz;
    c.sf = s->sf;
    c.bw_hz = s->bw;
    c.cr = s->cr;
    c.sync_word = s->word;
    c.preamble = 8;
    c.crc_on = true;
    c.implicit_rx_bytes = 0;
    c.cal_min_mhz = (uint16_t)(s->hz / 1000000u);
    c.cal_max_mhz = (uint16_t)(c.cal_min_mhz + 1);
    esp_err_t e = ls_lora_configure(&c);
    return e == ESP_OK ? ls_lora_receive() : e;
}

/* Open the next session the chip takes, from `from` on. */
static bool sess_next(int from, int64_t us)
{
    for (int k = 0; k < L.n; k++) {
        const int i = (from + k) % L.n;
        if (!sess_ok(&L.s[i], s_hw.caps)) continue;
        L.i = i;
        if (sess_begin(&L.s[i]) == ESP_OK) {
            L.open = true;
            L.sess_end = us + SESS_US;
            L.khz = L.s[i].hz / 1000;
            return true;
        }
    }
    return false;
}

static void sess_read(void)
{
    const sess_t *s = &L.s[L.i];
    wd_obs_t o;
    for (int k = 0; k < 4; k++) {
        float rssi = NAN, snr = 0;
        int n;
        if (s->kind == S_LORA) n = ls_lora_poll(s_lr_buf, sizeof(s_lr_buf), &rssi, &snr);
        else if (s->kind == S_FSK) n = ls_lora_fsk_poll(s_lr_buf, sizeof(s_lr_buf), &rssi);
        else n = ls_lora_ook_poll(s_lr_buf, sizeof(s_lr_buf), &rssi);
        if (n <= 0) return;
        if (s->kind == S_LORA) {
            if (wd_radio_obs_lora(&o, s_lr_buf, n, s->flavour, rssi, s->hz, WD_RADIO_LORA)) ring_put(&o);
            continue;
        }
        const lr433_capture_t cap = { .kind = s->kind == S_FSK ? LR433_IN_FSK : LR433_IN_OOK, .rate = s->rate,
                                      .lead = s->lead, .lead_bits = s->lead_bits, .data = s_lr_buf, .bits = n * 8 };
        const int m = lr433_decode(&cap, &s_lr_work, s_lr_msgs, 4);
        for (int j = 0; j < m; j++)
            if (wd_radio_obs_ism(&o, &s_lr_msgs[j], s->hz, rssi <= -199.0f ? NAN : rssi, WD_RADIO_LORA)) ring_put(&o);
    }
}

static void lora_service(void)
{
    const int job = L.job, region = L.region;
    const int64_t us = esp_timer_get_time();
    if (job != L.run_job || region != L.run_region) {
        sess_close();
        L.run_job = job;
        L.run_region = region;
        sessions(job, region, &L.s, &L.n);
        L.i = 0;
    }
    L.mc = false;
    if (job < 0 || job == WD_JOB_AIR || !L.n) {
        lora_close();
        L.busy[0] = 0;
        L.waiting = false;
        return;
    }
    if (job == WD_JOB_MESH && ls_mesh_running()) {
        lora_close();                       /* MeshCore listens; its adverts are read */
        L.mc = true;
        L.busy[0] = 0;
        L.waiting = false;
        return;
    }
    const char *who = ls_field_owned() ? "FIELD" : ls_lora_scanning() ? "a sweep" :
                      ls_mesh_radio_svc_yield() ? "a radio app" : NULL;
    if (who) {
        lora_close();
        snprintf(L.busy, sizeof(L.busy), "%s", who);
        L.waiting = false;
        return;
    }
    L.busy[0] = 0;
    if (!L.open) {
        L.asked = true;
        if (!ls_mesh_radio_hold_svc(true)) { L.waiting = true; return; }
        L.waiting = false;
        if (!sess_next(L.i, us)) { lora_close(); snprintf(L.busy, sizeof(L.busy), "chip refused"); }
        return;
    }
    if (!ls_mesh_radio_held_svc()) { lora_close(); L.waiting = true; return; }
    sess_read();
    if (L.n > 1 && us >= L.sess_end) {
        sess_close();
        if (!sess_next(L.i + 1, us)) lora_close();
    }
}

/* Nothing left to let go of: the worker may end. */
static bool lora_idle(void) { return L.job >= 0 || (!L.open && !L.asked); }

/* ---- the passive sources ---- */

#define ISM_Q 16
static EXT_RAM_BSS_ATTR rec_ism_msg_t s_ism_q[ISM_Q];
static volatile int s_ism_head, s_ism_tail;
static portMUX_TYPE s_ism_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ism_sub;

static void ism_sink(const rec_ism_msg_t *m, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_ism_mux);
    const int nx = (s_ism_head + 1) % ISM_Q;
    if (nx != s_ism_tail) { s_ism_q[s_ism_head] = *m; s_ism_head = nx; }
    portEXIT_CRITICAL(&s_ism_mux);
}

static int64_t s_air_seen[ADSB_MAX_TRACKED];
static uint32_t s_air_icao[ADSB_MAX_TRACKED];
static int64_t s_ext_seen_us;
static uint32_t s_mesh_seen, s_mesh_seq, s_passive_ms;
static EXT_RAM_BSS_ATTR ls_extnode_t s_ext[LS_EXTNODES_MAX];
static EXT_RAM_BSS_ATTR ls_mesh_peer_t s_peer;

void wd_rport_start(void)
{
    hw_refresh();
    if (!s_ism_sub) s_ism_sub = rec_ism_subscribe(ism_sink, NULL);
    s_ism_head = s_ism_tail = 0;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        s_air_seen[i] = a ? a->last_seen_us : 0;
        s_air_icao[i] = a ? a->icao : 0;
    }
    s_ext_seen_us = esp_timer_get_time();
    s_mesh_seen = ls_mesh_now();
    s_mesh_seq = ls_mesh_event_seq();
    L.head = L.tail = 0;
}

void wd_rport_stop(void)
{
    if (s_ism_sub) { rec_ism_unsubscribe(ism_sink, NULL); s_ism_sub = false; }
    L.job = -1;
    if (M.mode || M.listen) mode_release();
}

void wd_rport_probe(int r, wd_rport_t *o)
{
    hw_refresh();
    memset(o, 0, sizeof(*o));
    const bool air_lora = s_hw.adsb == LS_RSEL_LORA;
    const bool air_sdr = s_hw.adsb == LS_RSEL_SDR_RTL || s_hw.adsb == LS_RSEL_SDR_HACKRF || s_hw.adsb == LS_RSEL_ADSB_DUAL;
    if (r == WD_RJ_LORA) {
        o->present = s_hw.lora;
        snprintf(o->name, sizeof(o->name), "%s", s_hw.lora_name);
        if (s_hw.caps & (LS_LORA_CAP_FSK | LS_LORA_CAP_MODES_RX)) o->can |= 1 << WD_JOB_TPMS | 1 << WD_JOB_ISM;
        if (s_hw.caps & LS_LORA_CAP_LORA) o->can |= 1 << WD_JOB_MESH;
        /* ADS-B on the chip (LR2021 Mode S) when that is where ADS-B goes,
           and the receiver mode is not the SDR's for TPMS. */
        /* Only with no SDR plugged: the chip's ADS-B takes the receiver mode
           the SDR's TPMS/ISM need, and an idle SDR is worth more than the
           chip's aircraft (the ADS-B table still feeds AIR passively). */
        if (air_lora && !(s_hw.rtl || s_hw.hackrf) && (s_hw.caps & LS_LORA_CAP_MODES_RX) &&
            !(M.user == WD_RJ_SDR && M.job != WD_JOB_AIR))
            o->can |= 1 << WD_JOB_AIR;
        if (L.job == WD_JOB_AIR) {
            snprintf(o->busy, sizeof(o->busy), "%s", M.busy);
            o->waiting = M.waiting;
        } else {
            snprintf(o->busy, sizeof(o->busy), "%s", L.busy);
            o->waiting = L.waiting;
            o->freq_khz = L.khz;
        }
        return;
    }
    o->present = s_hw.rtl || s_hw.hackrf;
    snprintf(o->name, sizeof(o->name), "%s", s_hw.rtl ? "RTL-SDR" : "HackRF");
    if (air_sdr) o->can |= 1 << WD_JOB_AIR;
    if (!(M.user == WD_RJ_LORA && L.job == WD_JOB_AIR)) o->can |= 1 << WD_JOB_TPMS | 1 << WD_JOB_ISM;
    if (M.user == WD_RJ_SDR || M.busy[0]) {
        snprintf(o->busy, sizeof(o->busy), "%s", M.busy);
        o->waiting = M.waiting;
    }
    if (M.listen) {
        rec_ism_listen_t st;
        rec_ism_listen_status(&st);
        o->freq_khz = st.hz / 1000;
    }
}

bool wd_rport_run(int r, int job, int region, char *why, size_t n)
{
    if (r == WD_RJ_LORA) {
        if (job >= 0 && job != WD_JOB_AIR) {
            const sess_t *s;
            int k, ok = 0;
            sessions(job, region, &s, &k);
            hw_refresh();
            for (int i = 0; i < k; i++) ok += sess_ok(&s[i], s_hw.caps);
            if (!ok) { if (why) snprintf(why, n, "chip has no mode for it"); return false; }
        }
        L.region = region;
        L.job = job;
        if (job == WD_JOB_AIR) { M.job = job; M.region = region; M.user = r; }
        else if (M.user == r) { M.user = -1; if (M.mode) mode_release(); }
        return true;
    }
    if (job < 0) {
        if ((M.user == r || M.user < 0) && (M.mode || M.listen)) mode_release();
        return true;
    }
    if (M.mode && M.job != job) listen_off();
    M.job = job;
    M.region = region;
    M.user = r;
    M.plan_n = 0;
    if (job != WD_JOB_AIR) {
        uint32_t khz[4];
        const int k = wd_radio_job_freqs(job, region, khz);
        for (int i = 0; i < k; i++) M.plan[M.plan_n++] = khz[i] * 1000u;
    }
    return true;
}

int wd_rport_poll(int r, wd_obs_t *out, int cap, uint32_t now)
{
    (void)now;
    if (r == WD_RJ_SDR || (r == WD_RJ_LORA && L.job == WD_JOB_AIR)) {
        if (M.user == r) mode_keep(r);
        return 0;
    }
    int n = 0;
    while (n < cap && L.tail != L.head) {
        out[n++] = s_ring[L.tail];
        L.tail = (L.tail + 1) % (int)(sizeof(s_ring) / sizeof(s_ring[0]));
    }
    return n;
}

int wd_rport_passive(wd_obs_t *out, int cap, uint32_t now)
{
    int n = 0;
    /* rtl_433 decodes from any receiver */
    while (n < cap) {
        rec_ism_msg_t m;
        bool got = false;
        portENTER_CRITICAL(&s_ism_mux);
        if (s_ism_tail != s_ism_head) { m = s_ism_q[s_ism_tail]; s_ism_tail = (s_ism_tail + 1) % ISM_Q; got = true; }
        portEXIT_CRITICAL(&s_ism_mux);
        if (!got) break;
        const int radio = m.source == REC_SOURCE_SX1262 ? WD_RADIO_LORA : WD_RADIO_SDR;
        if (wd_radio_obs_ism(&out[n], &m.m, m.hz, m.rssi, radio)) n++;
    }
    if (now - s_passive_ms < 1000) return n;
    s_passive_ms = now;
    /* aircraft, whoever runs ADS-B */
    const adsb_source_t src = adsb_active_source();
    if (src != ADSB_SRC_NONE) {
        const int radio = src == ADSB_SRC_LORA || src == ADSB_SRC_DUAL ? WD_RADIO_LORA : WD_RADIO_SDR;
        for (int i = 0; i < ADSB_MAX_TRACKED && n < cap; i++) {
            const adsb_aircraft_t *a = adsb_state_get(i);
            if (!a || !a->active || !a->icao) continue;
            if (a->last_seen_us == s_air_seen[i] && a->icao == s_air_icao[i]) continue;
            s_air_seen[i] = a->last_seen_us;
            s_air_icao[i] = a->icao;
            wd_air_t w = { .icao = a->icao, .qual = a->address_qualifier,
                           .cat = (uint8_t)(a->emitter_tc << 4 | (a->emitter_ca & 15)),
                           .pos = a->pos_valid, .lat = a->lat, .lon = a->lon,
                           .alt_ft = a->altitude_valid ? a->altitude : INT32_MIN };
            memcpy(w.callsign, a->callsign, 8);
            w.callsign[8] = 0;
            wd_radio_obs_air(&out[n++], &w, radio);
        }
    }
    /* the MESHTASTIC app's nodes */
    const int k = ls_extnodes(s_ext, LS_EXTNODES_MAX);
    int64_t newest = s_ext_seen_us;
    for (int i = 0; i < k && n < cap; i++) {
        const ls_extnode_t *e = &s_ext[i];
        if (e->heard_us <= s_ext_seen_us) continue;
        if (e->heard_us > newest) newest = e->heard_us;
        wd_obs_t *o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->kind = WD_LORA;
        o->radio = WD_RADIO_LORA;
        o->sub = WD_MESH_MESHTASTIC;
        o->addr[0] = 'M'; o->addr[1] = 'T';
        o->addr[2] = (uint8_t)(e->id >> 24); o->addr[3] = (uint8_t)(e->id >> 16);
        o->addr[4] = (uint8_t)(e->id >> 8);  o->addr[5] = (uint8_t)e->id;
        snprintf(o->name, sizeof(o->name), "%s", e->name[0] ? e->name : e->short_name);
        if (!o->name[0]) snprintf(o->name, sizeof(o->name), "!%08x", (unsigned)e->id);
        const int lv = e->rssi_tenths / 10;
        o->rssi = (int8_t)(lv < -127 ? -127 : lv > 0 ? 0 : lv);
        if (e->has_pos && (e->lat != 0 || e->lon != 0)) { o->own_pos = true; o->lat = e->lat; o->lon = e->lon; }
    }
    s_ext_seen_us = newest;
    /* MeshCore's adverts: walked only after it has had an event */
    const uint32_t seq = ls_mesh_running() ? ls_mesh_event_seq() : s_mesh_seq;
    if (seq != s_mesh_seq) {
        s_mesh_seq = seq;
        uint32_t top = s_mesh_seen;
        for (int rank = 0; rank < 8 && n < cap && ls_mesh_peer_at(rank, &s_peer); rank++) {
            if (s_peer.last_heard <= s_mesh_seen) break;
            if (s_peer.last_heard > top) top = s_peer.last_heard;
            wd_obs_t *o = &out[n++];
            memset(o, 0, sizeof(*o));
            o->kind = WD_LORA;
            o->radio = WD_RADIO_LORA;
            o->sub = WD_MESH_MESHCORE;
            memcpy(o->addr, s_peer.pub_key, 6);
            snprintf(o->name, sizeof(o->name), "%s", s_peer.name[0] ? s_peer.name : s_peer.id);
            const float rs = s_peer.rssi;
            o->rssi = (int8_t)(!(rs > -127) ? -127 : rs > 0 ? 0 : (int)rs);
            if (s_peer.type == LS_MESH_ROLE_REPEATER || s_peer.type == LS_MESH_ROLE_ROOM) o->tags = WD_T_INFRA;
            if (s_peer.has_loc && (s_peer.lat_e6 || s_peer.lon_e6)) {
                o->own_pos = true; o->lat = s_peer.lat_e6 / 1e6; o->lon = s_peer.lon_e6 / 1e6;
            }
        }
        s_mesh_seen = top;
    }
    return n;
}
