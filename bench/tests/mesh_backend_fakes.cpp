/* Hardware and crypto fixtures. These tests validate the port, not encryption. */
#include "Utils.h"
#include <string.h>
#include <map>
#include <string>
#include <vector>
extern "C" {
#include "nvs.h"
#include "ls_nvs_safe.h"
#include "ls_lora.h"
#include "ls_gps.h"
#include "ls_rlog.h"
#include "ls_gauge.h"
#include "ls_track.h"
#include "ls_track_log.h"
}
int mesh_fake_rx_error;
bool mesh_fake_send_done = true;
int mesh_fake_send_error;
static bool receiving;
static ls_lora_cfg_t config;
static std::map<std::string, std::vector<uint8_t>> settings;
extern "C" {
void esp_restart(void) {}
esp_err_t ls_nvs_call(ls_nvs_fn_t fn, void *arg, unsigned) { return fn(arg); }
esp_err_t ls_nvs_run(ls_nvs_fn_t fn, void *arg, unsigned) { return fn(arg); }
esp_err_t nvs_open(const char *, int, nvs_handle_t *h) { *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_blob(nvs_handle_t, const char *key, void *out, size_t *len) {
    auto it = settings.find(key); if (it == settings.end()) return ESP_ERR_NOT_FOUND;
    if (*len < it->second.size()) return ESP_ERR_INVALID_SIZE;
    *len = it->second.size(); memcpy(out, it->second.data(), *len); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char *key, const void *val, size_t len) {
    const auto *p = static_cast<const uint8_t *>(val); settings[key] = {p, p + len}; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) { return nvs_get_blob(h, key, out, len); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val) { return nvs_set_blob(h, key, val, strlen(val) + 1); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) { size_t n = 1; return nvs_get_blob(h, key, out, &n); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val) { return nvs_set_blob(h, key, &val, 1); }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) {
    memset(out, 0, sizeof(*out)); out->freq_hz = 910525000; out->sf = 7;
    out->bw_hz = 62500; out->power_dbm = 22; out->cr = 5; out->sync_word = 0x12;
}
esp_err_t ls_lora_configure(const ls_lora_cfg_t *c) { config = *c; receiving = false; return ESP_OK; }
const ls_lora_cfg_t *ls_lora_cfg(void) { return &config; }
esp_err_t ls_lora_receive(void) { receiving = !mesh_fake_rx_error; return mesh_fake_rx_error; }
bool ls_lora_is_receiving(void) { return receiving; }
esp_err_t ls_lora_park(void) { receiving = false; return ESP_OK; }
esp_err_t ls_lora_send(const uint8_t *, size_t) { receiving = false; return mesh_fake_send_error; }
bool ls_lora_send_done(void) { return mesh_fake_send_done; }
int ls_lora_poll(uint8_t *, size_t, float *, float *) { return 0; }
esp_err_t ls_lora_rssi_inst(float *out) { *out = -100; return ESP_OK; }
uint32_t ls_lora_airtime_ms(int) { return 10; }
bool ls_lora_fsk_active(void) { return false; }
const char *ls_lora_chip_name(void) { return "SX126x"; }
bool ls_lora_present(void) { return true; }
bool ls_lora_is_transmitting(void) { return false; }
bool ls_lora_modes_active(void) { return false; }
bool ls_lora_ook_active(void) { return false; }
bool ls_lora_scanning(void) { return false; }
void ls_gps_get(ls_gps_state_t *out) { memset(out, 0, sizeof(*out)); }
bool ls_gauge_get(ls_gauge_t *) { return false; }
float ls_track_distance_m(int32_t, int32_t, int32_t, int32_t) { return 0; }
int ls_track_gpx_waypoint(char *, size_t, const ls_track_pt_t *, const char *, const char *) { return 0; }
void ls_track_set_waypoint_source(ls_track_wpt_fn) {}
}
namespace mesh {
uint32_t RNG::nextInt(uint32_t low, uint32_t high) { return high > low ? low : high; }
void Utils::sha256(uint8_t *out, size_t n, const uint8_t *data, int len) {
    uint32_t h = 2166136261u; for (int i = 0; i < len; i++) h = (h ^ data[i]) * 16777619u;
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(h >> ((i % 4) * 8));
}
void Utils::sha256(uint8_t *out, size_t n, const uint8_t *a, int alen, const uint8_t *b, int blen) {
    std::vector<uint8_t> data(a, a + alen); data.insert(data.end(), b, b + blen);
    sha256(out, n, data.data(), (int)data.size());
}
int Utils::encryptThenMAC(const uint8_t *, uint8_t *out, const uint8_t *in, int n) {
    int len = (n + 15) & ~15; memset(out, 0, len + 2); memcpy(out + 2, in, n); return len + 2;
}
int Utils::MACThenDecrypt(const uint8_t *, uint8_t *out, const uint8_t *in, int n) {
    if (n < 2) return 0; memcpy(out, in + 2, n - 2); return n - 2;
}
void Utils::toHex(char *out, const uint8_t *in, size_t n) {
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02X", in[i]);
}
bool Utils::fromHex(uint8_t *out, int n, const char *in) {
    if ((int)strlen(in) != n * 2) return false;
    for (int i = 0; i < n; i++) { unsigned x; if (sscanf(in + i * 2, "%2x", &x) != 1) return false; out[i] = x; }
    return true;
}
void Utils::printHex(Stream&, const uint8_t *, size_t) {}
}

static std::map<std::string, std::vector<uint8_t>> logs;
extern "C" {
bool ls_rlog_open(ls_rlog_t *lg, const char *path, uint32_t size, uint32_t cap) {
    snprintf(lg->path, sizeof(lg->path), "%s", path); lg->rec_size = size;
    lg->capacity = cap; lg->open = true; lg->count = (uint32_t)(logs[path].size() / size); return true;
}
bool ls_rlog_append(ls_rlog_t *lg, const void *rec) {
    auto &data = logs[lg->path]; const auto *p = static_cast<const uint8_t *>(rec);
    data.insert(data.end(), p, p + lg->rec_size);
    if (data.size() > lg->capacity * lg->rec_size) data.erase(data.begin(), data.begin() + lg->rec_size);
    lg->count = (uint32_t)(data.size() / lg->rec_size); return true;
}
int ls_rlog_read_at(ls_rlog_t *lg, int i, void *out) {
    auto &data = logs[lg->path]; if (i < 0 || (size_t)(i + 1) * lg->rec_size > data.size()) return 0;
    memcpy(out, data.data() + i * lg->rec_size, lg->rec_size); return 1;
}
int ls_rlog_read(ls_rlog_t *lg, void *out, int max) {
    int n = 0; while (n < max && ls_rlog_read_at(lg, n, static_cast<uint8_t *>(out) + n * lg->rec_size)) n++;
    return n;
}
int ls_rlog_count(const ls_rlog_t *lg) { return lg->count; }
bool ls_rlog_clear(ls_rlog_t *lg) { logs[lg->path].clear(); lg->count = 0; return true; }
bool ls_rlog_replace(ls_rlog_t *lg, const void *data, int n) {
    ls_rlog_clear(lg); for (int i = 0; i < n; i++) ls_rlog_append(lg, static_cast<const uint8_t *>(data) + i * lg->rec_size);
    return true;
}
}

void mesh_fake_reset(void)
{
    settings.clear(); logs.clear(); receiving = false;
    mesh_fake_rx_error = mesh_fake_send_error = 0; mesh_fake_send_done = true;
}
