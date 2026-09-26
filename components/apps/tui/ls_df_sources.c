#include "ls_df_sources.h"
#include "ls_trail.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ls_df.h"
#include "ls_field.h"
#include "ls_ble_heard.h"
#include "ls_wireless.h"
#include "ls_value.h"
#include "ls_mesh.h"
#include "ls_mixrf.h"
#include "ls_lora.h"
#include "radio/radio_endpoint.h"

#define FRESH_US     1500000
#define SDR_OFFSET   200000        /* tune beside the target, away from DC */
#define SDR_PAIRS    2048
#define SDR_BAND     25000
#define SDR_DC       20000         /* closer than this to the centre is the DC spike */
#define WIFI_SCAN_US 3000000
#define QUEUE        64
#define DWELL_MIN_MS 150

static const char *const NAMES[LS_DFS_COUNT] = {
    "MESH", "LORA", "RTL-SDR", "HACKRF", "CC1101", "NRF24", "WI-FI", "BLUETOOTH", "NFC", "GPS" };

typedef struct {
    bool active;
    ls_dfs_t source;
    uint32_t freq[LS_DFS_CHANNELS];
    int nch;
    uint32_t dwell_ms;
    int target;                     /* -1: everything the source hears */
    char target_key[40];            /* SSID, BLE address or mesh id */
    char target_name[40];
    float level;
    int64_t level_us;
    uint32_t updates;
    const char *unit;
    char status[96];
    bool retune;                    /* worker must re-apply the channels */
    int cur;                        /* channel being dwelt on, from the worker */
} slot_t;

static StaticSemaphore_t s_lock_memory;
static SemaphoreHandle_t s_lock;
EXT_RAM_BSS_ATTR static struct {
    slot_t slot[LS_DFS_SLOTS];
    ls_dfs_reading_t q[QUEUE];
    int qhead, qn;
} s;

/* Worker-only. The radios themselves: two slots never share one, which
   ls_dfs_conflict makes sure of, so one of each is enough. */
static ls_radio_session_t *s_session;
static bool s_streaming, s_hackrf;
static uint64_t s_sdr_centre;
static int s_sdr_flush;            /* captures still to drop: they predate a change */
/* Gain in tenths of a dB, stepped so the strongest channel sits well
   inside the converter's range. A transmitter a few metres away pinned a
   fixed 29.7 dB at full scale in every direction, which leaves no bearing
   to find. Levels are reported with the gain taken off, so a step does
   not move the lobe. */
static int s_sdr_gain;
static int64_t s_sdr_gain_us;
/* The last capture with anything strong in it. A keyed transmitter's gaps
   look quiet one capture at a time; stepping up on one of those put the
   gain straight back into the burst. */
static int64_t s_sdr_warm_us;
#define SDR_GAIN_STEP 100
#define SDR_HOT_DBFS  (-8.0f)      /* this close to full scale: step down */
#define SDR_COLD_DBFS (-42.0f)     /* everything under this: step up, slowly */
#define SDR_UP_US     3000000
static uint8_t *s_iq;
static int64_t s_wifi_scan_us;
/* Worker-only, per slot. */
typedef struct {
    ls_dfs_t running;
    uint32_t heard;                 /* mesh packets, adverts, mixrf samples */
    uint32_t field_seq, wifi_rev, wifi_ms;
    int cur;
    int64_t dwell_until;
    uint32_t tuned_hz;
    int settle;                     /* readings still to drop after a retune */
} work_t;
static work_t w[LS_DFS_SLOTS] = { { .running = LS_DFS_COUNT }, { .running = LS_DFS_COUNT } };

static void lock(void) { if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_memory); xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static bool slot_ok(int k) { return k >= 0 && k < LS_DFS_SLOTS; }

static void publish(int k, int ch, uint32_t hz, float level, const char *unit, uint8_t flags)
{
    const int64_t now = esp_timer_get_time();
    lock();
    slot_t *sl = &s.slot[k];
    sl->level = level; sl->unit = unit; sl->level_us = now; sl->updates++;
    ls_dfs_reading_t *r = &s.q[(s.qhead + s.qn) % QUEUE];
    if (s.qn == QUEUE) s.qhead = (s.qhead + 1) % QUEUE;     /* the oldest goes */
    else s.qn++;
    *r = (ls_dfs_reading_t){ .us = now, .freq_hz = hz, .level = level, .slot = (uint8_t)k,
                             .channel = (uint8_t)ch, .flags = flags };
    unlock();
}

static void status(int k, const char *text) { lock(); snprintf(s.slot[k].status, sizeof(s.slot[k].status), "%s", text); unlock(); }

const char *ls_dfs_name(ls_dfs_t src) { return src >= 0 && src < LS_DFS_COUNT ? NAMES[src] : "?"; }

static bool endpoint_present(const char *id)
{
    ls_radio_endpoint_info_t info;
    return ls_radio_endpoint_get(id, &info) == LS_RADIO_OK && info.present;
}

const char *ls_dfs_unavailable(ls_dfs_t src)
{
    EXT_RAM_BSS_ATTR static ls_wireless_snapshot_t ws;
    EXT_RAM_BSS_ATTR static ls_mixrf_status_t m;
    switch (src) {
    case LS_DFS_MESH: { ls_mesh_stats_t st; ls_mesh_get_stats(&st);
        return st.running ? NULL : "MeshCore is off: open MESH to start it"; }
    case LS_DFS_LORA:   return ls_lora_present() ? NULL : "No SX1262 LoRa radio answered at boot";
    case LS_DFS_RTL:    return endpoint_present(LS_RADIO_ENDPOINT_RTL_USB) ? NULL : "Plug an RTL-SDR into the USB-A port";
    case LS_DFS_HACKRF: return endpoint_present(LS_RADIO_ENDPOINT_HACKRF_USB) ? NULL : "Plug a HackRF into the USB-A port";
    case LS_DFS_CC1101: ls_mixrf_snapshot(&m); return m.cc ? NULL : "Needs the T-MixRF keyboard board (CC1101)";
    case LS_DFS_NRF24:  ls_mixrf_snapshot(&m); return m.nrf ? NULL : "Needs the T-MixRF keyboard board (nRF24)";
    case LS_DFS_WIFI:   ls_wireless_get(&ws); return ws.wifi_available ? NULL : "Wi-Fi co-processor is not answering";
    case LS_DFS_BLE:    ls_wireless_get(&ws); return ws.bt_available ? NULL : "Bluetooth is off: turn it on in RADIOS";
    case LS_DFS_NFC:    return "NFC reaches a few centimetres: hold the board to the tag";
    case LS_DFS_GPS:    return "Satellites are overhead: GPS places each bearing instead";
    default: return "Unknown source";
    }
}

/* Radios that share hardware: the SX1262 (Mesh runs on it), the USB-A
   port, and the keyboard board's SPI bus. */
static int hardware(ls_dfs_t src)
{
    switch (src) {
    case LS_DFS_MESH: case LS_DFS_LORA: return 0;
    case LS_DFS_RTL: case LS_DFS_HACKRF: return 1;
    case LS_DFS_CC1101: case LS_DFS_NRF24: return 2;
    case LS_DFS_WIFI: return 3;
    case LS_DFS_BLE: return 4;
    default: return -1;
    }
}

const char *ls_dfs_conflict(int slot, ls_dfs_t src)
{
    if (!slot_ok(slot)) return "No such slot";
    lock();
    const slot_t *o = &s.slot[1 - slot];
    const bool clash = o->active && hardware(o->source) >= 0 && hardware(o->source) == hardware(src);
    const ls_dfs_t other = o->source;
    unlock();
    if (!clash) return NULL;
    static const char *const WHY[] = { "shares the SX1262 with", "shares the USB-A port with",
                                       "shares the keyboard radio bus with", "is already", "is already" };
    EXT_RAM_BSS_ATTR static char line[64];
    snprintf(line, sizeof(line), "%s %s the other slot's %s", ls_dfs_name(src), WHY[hardware(src)], ls_dfs_name(other));
    return line;
}

bool ls_dfs_in_range(ls_dfs_t src, uint32_t hz)
{
    switch (src) {
    case LS_DFS_LORA:   return hz >= 150000000u && hz <= 960000000u;
    case LS_DFS_RTL:    return hz >= 24000000u + SDR_OFFSET && hz <= 1766000000u;
    case LS_DFS_HACKRF: return hz >= 1000000u + SDR_OFFSET && hz <= 6000000000u;
    case LS_DFS_CC1101: return (hz >= 300000000u && hz <= 348000000u) || (hz >= 387000000u && hz <= 464000000u) ||
                               (hz >= 779000000u && hz <= 928000000u);
    case LS_DFS_NRF24:  return hz >= 2400000000u && hz < 2400000000u + LS_MIXRF_CHANNELS * 1000000u;
    default: return false;
    }
}

static bool tunable(ls_dfs_t src)
{
    return src == LS_DFS_LORA || src == LS_DFS_RTL || src == LS_DFS_HACKRF || src == LS_DFS_CC1101 || src == LS_DFS_NRF24;
}

/* Wi-Fi and Bluetooth observation is one switch on the co-processor: on
   while either slot wants it. */
static void wireless_refresh(void)
{
    lock();
    bool want = false;
    for (int k = 0; k < LS_DFS_SLOTS; k++)
        want |= s.slot[k].active && (s.slot[k].source == LS_DFS_WIFI || s.slot[k].source == LS_DFS_BLE);
    unlock();
    ls_wireless_observe(want);
}

/* ------------------------------------------------------------ selection -- */

static void sdr_close(void)
{
    if (s_session) {
        if (s_streaming) ls_radio_iq_stop(s_session);
        ls_radio_release(s_session);
    }
    s_session = NULL; s_streaming = false;
}

bool ls_dfs_select_slot(int k, ls_dfs_t src, uint32_t freq_hz)
{
    if (!slot_ok(k) || src < 0 || src >= LS_DFS_COUNT || ls_dfs_unavailable(src) || ls_dfs_conflict(k, src)) return false;
    lock();
    slot_t *sl = &s.slot[k];
    const bool changed = !sl->active || sl->source != src;
    sl->active = true; sl->source = src; sl->retune = true;
    if (freq_hz && (!tunable(src) || ls_dfs_in_range(src, freq_hz))) { sl->freq[0] = freq_hz; if (changed || !sl->nch) sl->nch = 1; }
    if (!sl->nch) sl->nch = 1;
    if (!sl->dwell_ms) sl->dwell_ms = 400;
    /* A list tuned for another radio may not fit this one. */
    if (changed && tunable(src)) {
        int n = 0;
        for (int i = 0; i < sl->nch; i++) if (ls_dfs_in_range(src, sl->freq[i])) sl->freq[n++] = sl->freq[i];
        if (!n) {
            static const uint32_t TRY[] = { 915000000u, 433920000u, 462562500u, 2440000000u };
            for (unsigned i = 0; i < sizeof(TRY) / sizeof(TRY[0]) && !n; i++)
                if (ls_dfs_in_range(src, TRY[i])) { sl->freq[0] = TRY[i]; n = 1; }
        }
        sl->nch = n ? n : 1;
    }
    if (changed) { sl->target = -1; sl->target_key[0] = sl->target_name[0] = 0; sl->level_us = 0; sl->status[0] = 0; sl->cur = 0; }
    unlock();
    if (src == LS_DFS_BLE) ls_wireless_request(LS_WIRELESS_BT_RESCAN, NULL, NULL);
    wireless_refresh();
    return true;
}

bool ls_dfs_select(ls_dfs_t src, uint32_t freq_hz) { return ls_dfs_select_slot(0, src, freq_hz); }

int ls_dfs_set_channels(int k, const uint32_t *hz, int n, uint32_t dwell_ms)
{
    if (!slot_ok(k) || !hz || n < 1) return 0;
    lock();
    slot_t *sl = &s.slot[k];
    int kept = 0;
    for (int i = 0; i < n && kept < LS_DFS_CHANNELS; i++)
        if (!sl->active || !tunable(sl->source) || ls_dfs_in_range(sl->source, hz[i])) sl->freq[kept++] = hz[i];
    if (kept) {
        sl->nch = kept; sl->cur = 0; sl->retune = true;
        sl->dwell_ms = dwell_ms < DWELL_MIN_MS ? DWELL_MIN_MS : dwell_ms;
    }
    unlock();
    return kept;
}

bool ls_dfs_tune(uint32_t freq_hz) { return freq_hz && ls_dfs_set_channels(0, &freq_hz, 1, s.slot[0].dwell_ms) == 1; }

void ls_dfs_stop_slot(int k)
{
    if (!slot_ok(k)) return;
    lock(); s.slot[k].active = false; s.slot[k].retune = true; unlock();
    wireless_refresh();
}

void ls_dfs_stop(void) { ls_dfs_stop_slot(0); }

void ls_dfs_poll_slot(int k, ls_dfs_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!slot_ok(k)) { out->source = LS_DFS_COUNT; return; }
    lock();
    const slot_t *sl = &s.slot[k];
    out->source = sl->source; out->active = sl->active;
    out->fresh = sl->level_us && esp_timer_get_time() - sl->level_us < FRESH_US;
    out->level = sl->level; out->unit = sl->unit ? sl->unit : "";
    out->freq_hz = sl->freq[0]; out->updates = sl->updates;
    out->channels = sl->nch ? sl->nch : 1; out->channel = sl->cur;
    memcpy(out->freqs, sl->freq, sizeof(out->freqs));
    snprintf(out->target, sizeof(out->target), "%s", sl->target_name[0] ? sl->target_name : "everything heard");
    snprintf(out->status, sizeof(out->status), "%s", sl->status);
    const ls_dfs_t src = sl->source;
    unlock();
    out->tunable = tunable(src);
    out->targets = src == LS_DFS_MESH || src == LS_DFS_WIFI || src == LS_DFS_BLE;
    if (!out->tunable) out->channels = 1;
}

void ls_dfs_poll(ls_dfs_status_t *out) { ls_dfs_poll_slot(0, out); }

int ls_dfs_take(ls_dfs_reading_t *out, int max)
{
    if (!out || max < 1) return 0;
    lock();
    int n = 0;
    while (s.qn && n < max) { out[n++] = s.q[s.qhead]; s.qhead = (s.qhead + 1) % QUEUE; s.qn--; }
    unlock();
    return n;
}

/* --------------------------------------------------------------- targets -- */

EXT_RAM_BSS_ATTR static ls_ble_heard_t s_ble[LS_BLE_HEARD_MAX];
EXT_RAM_BSS_ATTR static ls_wireless_snapshot_t s_wsnap;

int ls_dfs_target_count(void)
{
    lock(); const ls_dfs_t src = s.slot[0].source; unlock();
    if (src == LS_DFS_BLE) return ls_ble_heard_list(s_ble, LS_BLE_HEARD_MAX, esp_timer_get_time(), 30000000);
    if (src == LS_DFS_WIFI) { ls_wireless_get(&s_wsnap); return s_wsnap.ap_count; }
    if (src == LS_DFS_MESH) { int n = 0; ls_mesh_peer_t p; while (n < 32 && ls_mesh_peer_at(n, &p)) n++; return n; }
    return 0;
}

static void addr_text(const uint8_t a[6], char *out, size_t cap)
{
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

bool ls_dfs_target_label(int i, char *label, size_t lcap, char *detail, size_t dcap)
{
    lock(); const ls_dfs_t src = s.slot[0].source; unlock();
    if (src == LS_DFS_BLE && i >= 0 && i < LS_BLE_HEARD_MAX) {
        char a[20]; addr_text(s_ble[i].addr, a, sizeof(a));
        snprintf(label, lcap, "%s", s_ble[i].name[0] ? s_ble[i].name : a);
        snprintf(detail, dcap, "%s  %d dBm", a, s_ble[i].rssi);
        return true;
    }
    if (src == LS_DFS_WIFI && i >= 0 && i < s_wsnap.ap_count) {
        snprintf(label, lcap, "%s", s_wsnap.aps[i].ssid[0] ? s_wsnap.aps[i].ssid : "(hidden)");
        snprintf(detail, dcap, "ch %d  %d dBm", s_wsnap.aps[i].channel, s_wsnap.aps[i].rssi);
        return true;
    }
    if (src == LS_DFS_MESH) {
        ls_mesh_peer_t p;
        if (!ls_mesh_peer_at(i, &p)) return false;
        snprintf(label, lcap, "%s", p.name[0] ? p.name : p.id);
        snprintf(detail, dcap, "%.8s  %.0f dBm%s", p.id, p.rssi, p.has_loc ? "  has position" : "");
        return true;
    }
    return false;
}

bool ls_dfs_target_pick(int i)
{
    char label[40] = "", detail[48];
    lock(); const ls_dfs_t src = s.slot[0].source; unlock();
    char key[40] = "";
    if (i >= 0) {
        if (!ls_dfs_target_label(i, label, sizeof(label), detail, sizeof(detail))) return false;
        if (src == LS_DFS_BLE) memcpy(key, s_ble[i].addr, 6);
        else if (src == LS_DFS_WIFI) snprintf(key, sizeof(key), "%s", s_wsnap.aps[i].ssid);
        else if (src == LS_DFS_MESH) { ls_mesh_peer_t p; if (ls_mesh_peer_at(i, &p)) snprintf(key, sizeof(key), "%s", p.id); }
    }
    lock();
    slot_t *sl = &s.slot[0];
    sl->target = i; memcpy(sl->target_key, key, sizeof(key));
    snprintf(sl->target_name, sizeof(sl->target_name), "%s", label);
    sl->level_us = 0;
    unlock();
    return true;
}

/* ---------------------------------------------------------------- worker -- */

static void set_cur(int k, int cur) { lock(); s.slot[k].cur = cur; unlock(); }

/* The next channel on a dwell: the list goes round. */
static void dwell_next(int k, const slot_t *c, int64_t now)
{
    if (c->nch > 1 && now >= w[k].dwell_until) {
        w[k].cur = (w[k].cur + 1) % c->nch;
        set_cur(k, w[k].cur);
    }
}

static void step_mesh(int k, const char *key, bool targeted)
{
    if (targeted) {
        ls_mesh_peer_t p;
        for (int r = 0; r < 32 && ls_mesh_peer_at(r, &p); r++)
            if (!strcmp(p.id, key)) {
                /* A peer's level changes only when it is heard again. */
                if (p.adverts != w[k].heard) { w[k].heard = p.adverts; publish(k, 0, 0, p.rssi, "dBm", 0); }
                return;
            }
        return;
    }
    ls_mesh_stats_t st; ls_mesh_get_stats(&st);
    if (st.rx_packets != w[k].heard) { w[k].heard = st.rx_packets; publish(k, 0, 0, st.last_rssi, "dBm", 0); }
}

static void step_lora(int k, const slot_t *c, bool retune)
{
    EXT_RAM_BSS_ATTR static ls_field_state_t f;
    const int64_t now = esp_timer_get_time();
    if (retune) {
        ls_field_mode(LS_LAB_PACKETS);
        ls_field_direct(true);
        w[k].tuned_hz = 0;
        status(k, "Taking the LoRa radio from Mesh");
    }
    const uint32_t want = c->freq[w[k].cur];
    if (w[k].tuned_hz != want) {
        ls_field_snapshot(&f);
        ls_lora_cfg_t cfg = f.config;
        if (want) cfg.freq_hz = want;
        ls_field_configure(&cfg);
        w[k].tuned_hz = want;
        w[k].field_seq = f.sequence;
        w[k].settle = 1;
        w[k].dwell_until = now + (int64_t)c->dwell_ms * 1000;
    }
    ls_field_snapshot(&f);
    if (!f.direct || f.config.freq_hz != want) return;       /* not applied yet */
    if (f.sequence != w[k].field_seq) {
        w[k].field_seq = f.sequence;
        const float v = f.trace[LS_FIELD_BINS - 1];
        if (w[k].settle > 0) w[k].settle--;
        else if (isfinite(v) && v > -139) {
            publish(k, w[k].cur, want, v, "dBm", 0);
            status(k, c->nch > 1 ? "Carrier level, channel by channel" : "Carrier level at the tuned frequency");
        }
    }
    dwell_next(k, c, now);
}

static const char *borrowed_value(float *level, uint32_t *freq)
{
    /* The SDR is a radio app's: follow that app's own channel level. */
    static const char *const APPS[][3] = { { "p25.level", "p25.freq", "P25" }, { "fm.level", "fm.freq", "FM" } };
    for (unsigned i = 0; i < 2; i++) {
        ls_val_t v, f;
        if (ls_value_read(APPS[i][0], &v, NULL) && v.kind == LS_VAL_FLOAT && v.f != 0 &&
            ls_value_read(APPS[i][1], &f, NULL) && f.kind == LS_VAL_FLOAT && f.f > 0) {
            *level = v.f > 0 && v.f <= 1 ? 20.0f * log10f(v.f) : v.f;
            *freq = (uint32_t)(f.f * 1e6f);
            return APPS[i][2];
        }
    }
    return NULL;
}

/* How far from the centre one capture measures a channel well. */
static int64_t sdr_reach(bool hackrf) { return hackrf ? 800000 : 400000; }

static bool sdr_covers(uint64_t centre, uint32_t hz, bool hackrf)
{
    const int64_t off = (int64_t)hz - (int64_t)centre;
    const int64_t a = off < 0 ? -off : off;
    return a >= SDR_DC && a <= sdr_reach(hackrf);
}

static void step_sdr(int k, const slot_t *c, bool hackrf, bool retune)
{
    if (retune || hackrf != s_hackrf) sdr_close();
    if (hackrf != s_hackrf) s_sdr_gain = 0;        /* the other radio's scale */
    s_hackrf = hackrf;
    const uint32_t rate = hackrf ? 2000000 : 1024000;
    const uint32_t first = c->freq[w[k].cur];
    if (!first) { status(k, "Set a frequency to measure"); return; }
    if (!s_session) {
        const uint64_t centre = first - SDR_OFFSET;
        ls_radio_requirements_t req = { .required_caps = LS_RADIO_RX_IQ_U8,
            .min_hz = centre, .max_hz = centre, .sample_rate_hz = rate,
            .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
            .preferred_endpoint_id = hackrf ? LS_RADIO_ENDPOINT_HACKRF_USB : LS_RADIO_ENDPOINT_RTL_USB };
        const ls_radio_err_t err = ls_radio_acquire("compass-df", &req, &s_session);
        if (err != LS_RADIO_OK) {
            s_session = NULL;
            float level; uint32_t f;
            const char *app = borrowed_value(&level, &f);
            if (app) {
                publish(k, 0, f, level, "dB", 0);
                char line[96]; snprintf(line, sizeof(line), "%s has the SDR: following its level at %.4f MHz", app, f / 1e6);
                status(k, line);
            } else {
                char line[96]; snprintf(line, sizeof(line), "SDR busy (%s); close the app using it", ls_radio_err_name(err));
                status(k, line);
            }
            return;
        }
        if (!s_sdr_gain) s_sdr_gain = hackrf ? 400 : 297;
        ls_radio_iq_config_t cfg = { .center_hz = centre, .sample_rate_hz = rate, .bandwidth_hz = rate,
            .gain_mode = LS_RADIO_GAIN_MANUAL, .gain_tenths_db = s_sdr_gain }, actual;
        if (ls_radio_iq_configure(s_session, &cfg, &actual) != LS_RADIO_OK || ls_radio_iq_start(s_session) != LS_RADIO_OK) {
            status(k, "The SDR would not tune there"); sdr_close(); return;
        }
        s_streaming = true; s_sdr_centre = centre;
        s_sdr_gain = actual.gain_tenths_db > 0 ? actual.gain_tenths_db : s_sdr_gain;
        s_sdr_gain_us = esp_timer_get_time();
        s_sdr_flush = 2;                   /* the tuner settles after a start */
        w[k].dwell_until = esp_timer_get_time() + (int64_t)c->dwell_ms * 1000;
        if (!s_iq) s_iq = heap_caps_malloc(2 * SDR_PAIRS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        status(k, "Measuring the channel");
    }
    if (!s_iq) { status(k, "No memory for samples"); return; }
    size_t got = 0, n = 0;
    while (got < 2 * SDR_PAIRS) {
        if (ls_radio_iq_read(s_session, s_iq + got, 2 * SDR_PAIRS - got, 100, &n) != LS_RADIO_OK || !n) break;
        got += n;
    }
    if (got < 2 * SDR_PAIRS) { status(k, "The SDR stopped delivering samples"); sdr_close(); return; }
    /* Samples queued before a change belong to the old centre or gain. */
    if (s_sdr_flush > 0) { s_sdr_flush--; return; }
    /* A rail-to-rail sample says the front end is overloaded, and near
       the transmitter that flattens the very peak being looked for. */
    int rail = 0;
    for (int i = 0; i < 2 * SDR_PAIRS; i += 8) rail += s_iq[i] == 0 || s_iq[i] == 255;
    const bool overload = rail > SDR_PAIRS / 32;
    /* Every channel this capture reaches, from the one capture, with the
       gain taken off. */
    int measured = 0;
    float hottest = -200.0f;
    const float gain_db = s_sdr_gain / 10.0f;
    for (int i = 0; i < c->nch; i++) {
        if (!sdr_covers(s_sdr_centre, c->freq[i], hackrf)) continue;
        const int32_t off = (int32_t)((int64_t)c->freq[i] - (int64_t)s_sdr_centre);
        const float dbfs = ls_df_iq_band_db(s_iq, SDR_PAIRS, rate, off, SDR_BAND);
        if (dbfs > hottest) hottest = dbfs;
        publish(k, i, c->freq[i], dbfs - gain_db, "dB", overload ? LS_DFS_READ_OVERLOAD : 0);
        measured++;
    }
    const int64_t now = esp_timer_get_time();
    if (overload || hottest > SDR_COLD_DBFS) s_sdr_warm_us = now;
    /* Down at once when hot, up only after a quiet spell, so it does not
       hunt; the capture after a step is dropped. */
    const int max_gain = hackrf ? 620 : 496;
    int want = s_sdr_gain;
    if ((overload || hottest > SDR_HOT_DBFS) && s_sdr_gain > 0) want = s_sdr_gain - SDR_GAIN_STEP;
    else if (now - s_sdr_warm_us > SDR_UP_US && s_sdr_gain < max_gain && now - s_sdr_gain_us > SDR_UP_US) want = s_sdr_gain + SDR_GAIN_STEP;
    if (want < 0) want = 0;
    if (want > max_gain) want = max_gain;
    if (want != s_sdr_gain) {
        int got = want;
        if (ls_radio_iq_set_gain(s_session, LS_RADIO_GAIN_MANUAL, want, &got) == LS_RADIO_OK) {
            s_sdr_gain = got; s_sdr_flush = 1;
        }
        s_sdr_gain_us = now;
    }
    char line[96];
    if (overload && s_sdr_gain == 0) snprintf(line, sizeof(line), "OVERLOADED at 0 dB gain: step away or use a smaller antenna");
    else if (c->nch > 1) snprintf(line, sizeof(line), "%d of %d channels per capture, gain %.1f dB", measured, c->nch, s_sdr_gain / 10.0);
    else snprintf(line, sizeof(line), "Channel level, 25 kHz wide, gain %.1f dB", s_sdr_gain / 10.0);
    status(k, line);
    /* The rest of the list needs another centre: go there after a dwell. */
    if (measured < c->nch && now >= w[k].dwell_until) {
        int next = w[k].cur;
        for (int step = 1; step <= c->nch; step++) {
            const int j = (w[k].cur + step) % c->nch;
            if (!sdr_covers(s_sdr_centre, c->freq[j], hackrf)) { next = j; break; }
        }
        uint64_t actual = 0;
        const uint64_t centre = c->freq[next] - SDR_OFFSET;
        if (ls_radio_iq_retune(s_session, centre, true, &actual) == LS_RADIO_OK) {
            s_sdr_centre = actual ? actual : centre;
            s_sdr_flush = 2;
            w[k].cur = next; set_cur(k, next);
        }
        w[k].dwell_until = now + (int64_t)c->dwell_ms * 1000;
    }
}

static void step_mixrf(int k, const slot_t *c, bool nrf, bool retune)
{
    EXT_RAM_BSS_ATTR static ls_mixrf_status_t m;
    const int64_t now = esp_timer_get_time();
    if (retune) {
        ls_mixrf_start();
        if (nrf) ls_mixrf_scan(true);
        w[k].tuned_hz = 0; w[k].heard = 0;
    }
    if (nrf) {
        ls_mixrf_snapshot(&m);
        if (m.samples == w[k].heard) return;
        w[k].heard = m.samples;
        /* One sweep hears every channel. */
        for (int i = 0; i < c->nch; i++) {
            const uint32_t hz = c->freq[i];
            int ch = hz >= 2400000000u ? (int)((hz - 2400000000u) / 1000000u) : 0;
            if (ch < 0 || ch >= LS_MIXRF_CHANNELS) ch = 0;
            publish(k, i, hz, m.occupancy[ch] * (100.0f / 255.0f), "%", 0);
        }
        return;
    }
    const uint32_t want = c->freq[w[k].cur];
    if (w[k].tuned_hz != want) {
        ls_mixrf_receive(true, want);
        w[k].tuned_hz = want; w[k].settle = 1;
        w[k].dwell_until = now + (int64_t)c->dwell_ms * 1000;
    }
    ls_mixrf_snapshot(&m);
    if (m.frequency != want || m.samples == w[k].heard) { dwell_next(k, c, now); return; }
    w[k].heard = m.samples;
    if (w[k].settle > 0) w[k].settle--;
    else publish(k, w[k].cur, want, m.rssi, "dBm", 0);
    dwell_next(k, c, now);
}

static void step_wifi(int k, const char *ssid, bool targeted)
{
    ls_wireless_get(&s_wsnap);
    if (!targeted) {
        if (s_wsnap.wifi_connected && s_wsnap.wifi_signal && s_wsnap.now_ms != w[k].wifi_ms) {
            w[k].wifi_ms = s_wsnap.now_ms; publish(k, 0, 0, (float)s_wsnap.wifi_rssi, "dBm", 0);
            status(k, "The network this board is joined to");
        } else if (!s_wsnap.wifi_connected) status(k, "Pick a network: not joined to one");
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (!s_wsnap.busy && now - s_wifi_scan_us > WIFI_SCAN_US) {
        s_wifi_scan_us = now; ls_wireless_request(LS_WIRELESS_SCAN, NULL, NULL);
    }
    if (s_wsnap.scan_revision == w[k].wifi_rev) return;
    w[k].wifi_rev = s_wsnap.scan_revision;
    for (int i = 0; i < s_wsnap.ap_count; i++)
        if (!strcmp(s_wsnap.aps[i].ssid, ssid)) {
            publish(k, 0, 0, (float)s_wsnap.aps[i].rssi, "dBm", 0);
            status(k, "One level per scan, about every 3 s: turn slowly");
            return;
        }
    status(k, "Network not in the last scan");
}

static void step_ble(int k, const uint8_t *addr, bool targeted)
{
    ls_wireless_get(&s_wsnap);
    if (!targeted) {
        if (s_wsnap.bt_ready && s_wsnap.bt_signal && s_wsnap.now_ms != w[k].wifi_ms) {
            w[k].wifi_ms = s_wsnap.now_ms; publish(k, 0, 0, (float)s_wsnap.bt_rssi, "dBm", 0);
            status(k, "The connected Bluetooth peer");
        } else status(k, "Pick a device heard advertising");
        return;
    }
    ls_ble_heard_t d;
    if (ls_ble_heard_find(addr, &d) && d.adverts != w[k].heard && esp_timer_get_time() - d.last_us < FRESH_US) {
        w[k].heard = d.adverts;
        publish(k, 0, 0, (float)d.rssi, "dBm", 0);
        status(k, "One level per advert heard");
    }
}

/* Give back whatever a slot's last source held. */
static void release(ls_dfs_t running, ls_dfs_t next, bool active)
{
    if (running == LS_DFS_RTL || running == LS_DFS_HACKRF) sdr_close();
    if (running == LS_DFS_LORA && (next != LS_DFS_LORA || !active)) ls_field_direct(false);
    if ((running == LS_DFS_CC1101 || running == LS_DFS_NRF24) && (next != running || !active)) {
        ls_mixrf_receive(false, 0); ls_mixrf_scan(false);
    }
}

void ls_dfs_step(void)
{
    ls_trail(LS_TRAIL_DFS, "step");
    EXT_RAM_BSS_ATTR static slot_t c[LS_DFS_SLOTS];
    for (int k = 0; k < LS_DFS_SLOTS; k++) {
        lock();
        c[k] = s.slot[k];
        s.slot[k].retune = false;
        unlock();
        const bool retune = c[k].retune;
        if (!c[k].active || c[k].source != w[k].running || retune) {
            release(w[k].running, c[k].source, c[k].active);
            w[k].running = c[k].active ? c[k].source : LS_DFS_COUNT;
            w[k].heard = w[k].field_seq = w[k].wifi_rev = 0;
            w[k].cur = 0; w[k].tuned_hz = 0; w[k].dwell_until = 0;
            if (c[k].active) set_cur(k, 0);
        }
        if (!c[k].active) continue;
        if (c[k].nch < 1) c[k].nch = 1;
        if (w[k].cur >= c[k].nch) w[k].cur = 0;
        const bool targeted = c[k].target >= 0;
        ls_trail(LS_TRAIL_DFS, ls_dfs_name(c[k].source));
        switch (c[k].source) {
        case LS_DFS_MESH:   step_mesh(k, c[k].target_key, targeted); break;
        case LS_DFS_LORA:   step_lora(k, &c[k], retune); break;
        case LS_DFS_RTL:    step_sdr(k, &c[k], false, retune); break;
        case LS_DFS_HACKRF: step_sdr(k, &c[k], true, retune); break;
        case LS_DFS_CC1101: step_mixrf(k, &c[k], false, retune); break;
        case LS_DFS_NRF24:  step_mixrf(k, &c[k], true, retune); break;
        case LS_DFS_WIFI:   step_wifi(k, c[k].target_key, targeted); break;
        case LS_DFS_BLE:    step_ble(k, (const uint8_t *)c[k].target_key, targeted); break;
        default: break;
        }
    }
}
