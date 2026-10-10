#ifndef LS_SWEEP_APP_H
#define LS_SWEEP_APP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SWEEP_SETTINGS_MAGIC UINT32_C(0x5F90DEDF)
#define SWEEP_SETTINGS_BUILD UINT32_C(0x9FEAC22E)
#define LS_SWEEP_CAP 256
#define LS_SWEEP_LIST_US INT64_C(60000000)
#define LS_SWEEP_RULES 64
#define LS_SWEEP_RULE_BYTES 16384
typedef enum { SW_CAMERA, SW_BODYCAM, SW_DRONE, SW_TRACKER, SW_ATTACK, SW_CATS } ls_sweep_cat_t;
typedef struct {
    uint8_t category;
    char label[48], vendor[24];
    bool immediate_alert;
    enum { SW_STATE_UNKNOWN, SW_STATE_SEPARATED, SW_STATE_NEAR } state;
} ls_sweep_match_t;
typedef struct {
    ls_sweep_match_t match;
    uint8_t mac[6], address_type, radio;
    int8_t rssi, trend[24];
    uint8_t samples;
    int64_t seen_us, alerted_us;
    uint32_t serial;
    int64_t first_us, separated_us, peak_us;
    int64_t bucket[24];
    int8_t history[24], median[5];
    uint16_t bucket_adverts[24];
    uint8_t median_n, median_at;
    float fast, slow, peak, moved_m;
    float fast_history[24], slow_history[24], slow_peak;
    int64_t slow_peak_us;
    unsigned places;
    bool with_you;
    uint32_t adverts;
} ls_sweep_device_t;
typedef struct {
    uint32_t version;
    uint8_t enabled, muted, filter;
    bool logging, receive_only, hunt;
    uint8_t hunt_mac[6], hunt_radio, hunt_type;
    /* Version 2 uses a former v1 padding byte; category mutes stay independent. */
    bool alerts_muted;
    /* v3 consumes the last v1/v2 padding byte; category enabled is separate. */
    bool on;
} ls_sweep_settings_t;
typedef struct {
    uint32_t magic, build;
    ls_sweep_settings_t value;
} ls_sweep_settings_blob_t;
typedef struct {
    bool requested, running, flood;
    unsigned rules, dropped, owner_nearby;
    float adverts_s;
    int64_t last_advert_us;
    bool gps_fix;
    char status[96];
} ls_sweep_status_t;
/* Lifetime counters; unique addresses are a rolling 60s BLE window, bounded
 * at 1024. Saturation is explicit, so addresses never imply physical devices. */
typedef struct {
    uint32_t adverts_in, accepted[SW_CATS], evicted_capacity, expiries;
    uint32_t category_changes, flood_entries, alerts_suppressed, unique_saturated;
    unsigned table_size, capacity, unique_addresses_min;
} ls_sweep_stats_t;
typedef struct {
    uint32_t adverts_in, dispatched, ring_drops, ring_overflow, invalid_reports;
    unsigned ring_size, ring_capacity, ring_high_water;
    bool available, filter_duplicates, scanning, while_connected;
    unsigned scan_interval_units, scan_window_units;
} ls_sweep_transport_stats_t;
typedef enum { SW_WITH_YOU, SW_NEW, SW_PASSING } ls_sweep_section_t;
ls_sweep_section_t ls_sweep_section(const ls_sweep_device_t *d, int64_t now, bool gps);
bool ls_sweep_alarm(const ls_sweep_device_t *d, bool gps);
int ls_sweep_bucket(const ls_sweep_device_t *d, int64_t now, unsigned age);
float ls_sweep_hunt_trend(const ls_sweep_device_t *d, int64_t now, bool slow);
float ls_sweep_trend(const ls_sweep_device_t *d, int64_t now);
float ls_sweep_rate(const ls_sweep_device_t *d, int64_t now);
/* Counts/distance only. Caller must supply validated GPS movement; no coordinates. */
void ls_sweep_motion(bool fix, float moved_m, unsigned new_places, int64_t now);
void ls_sweep_stats(ls_sweep_stats_t *out, int64_t now);
void ls_sweep_transport_stats(ls_sweep_transport_stats_t *out);
const char *ls_sweep_category(unsigned cat);
bool ls_sweep_parse_ble(const uint8_t *ad, size_t len, ls_sweep_match_t *out);
bool ls_sweep_parse_wifi(const uint8_t bssid[6], const char *ssid, ls_sweep_match_t *out);
/* Strict bounded JSON: array of objects with category,label,vendor and exactly
 * one selector (oui/company/name/uuid/ssid). Atomic replacement on success. */
bool ls_sweep_rules_parse(const char *json, size_t len);
bool ls_sweep_rules_reload(void);
void ls_sweep_defaults(void);
void ls_sweep_init(void);
void ls_sweep_rules_seed(void);
void ls_sweep_reload_request(void);
bool ls_sweep_reload_pending(void);
bool ls_sweep_log_event(ls_sweep_device_t *out);
void ls_sweep_settings_get(ls_sweep_settings_t *out);
bool ls_sweep_settings_set(const ls_sweep_settings_t *in);
void ls_sweep_settings_load(void);
void ls_sweep_start(bool on);
bool ls_sweep_pending(void);
void ls_sweep_runtime_status(bool running, const char *status);
void ls_sweep_status(ls_sweep_status_t *out, int64_t now);
void ls_sweep_ble(const uint8_t mac[6], uint8_t type, int rssi, const uint8_t *ad, size_t len, int64_t now);
void ls_sweep_wifi(const uint8_t mac[6], const char *ssid, int rssi, int64_t now);
void ls_sweep_observe(const uint8_t mac[6], uint8_t radio, uint8_t type, int rssi, const ls_sweep_match_t *match, int64_t now);
typedef struct {
    unsigned counts[SW_CATS];
    uint32_t serial[2];
    int rssi[2];
    uint8_t category[2];
    bool running, muted;
} ls_sweep_summary_t;
void ls_sweep_summary(ls_sweep_summary_t *out, int64_t now);
size_t ls_sweep_snapshot(ls_sweep_device_t *out, size_t cap, int64_t now);
bool ls_sweep_nearby_target(ls_sweep_device_t *out, int64_t now);
bool ls_sweep_hunt(const uint8_t mac[6], uint8_t radio, uint8_t type);
/* Worker consumes at most one category per interval. No audio in scan callback. */
int ls_sweep_alert(int64_t now);
int ls_sweep_command(int argc, char **argv);
void ls_sweep_clear(void);
/* Platform hooks; host builds have inert defaults. */
void ls_sweep_wake(void);
void ls_sweep_tick(void);
#ifdef __cplusplus
}
#endif
#endif
