/* The LSX link as a service (ls_lsx_svc.h): the LoRa chip as the engine's
   phy, the task, NVS settings and the 'lsx' command. On the host bench the
   same calls run a simulated home base and field unit over the loopback,
   so lssim's desk window shows a real exchange. */
#include "ls_lsx_svc.h"
#include "ls_lsx_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    ls_lsx_t *x;
    ls_lsx_sync_t *sync;
    int role;
    const char *why;
    const ls_lsx_store_t *store_ext, *store_test;
    uint8_t key[16];
    bool key_set, loaded;
    uint8_t region, max_mode, drop;
    int8_t power;
    uint8_t autorole;
    uint32_t id;
    char name[LSX_NAME];
    ls_lsx_party_rx_t party_rx;     /* PARTY: where heard datagrams go */
    void *party_ctx;
} S = { .why = "stopped", .max_mode = LSX_M_N - 1, .power = 14 };

__attribute__((weak)) const ls_lsx_store_t *ls_lsx_default_store(void) { return NULL; }

/* PARTY's app: no sessions, every datagram to the party with the level the
   engine just measured (it records each frame's RSSI and SNR before handing
   the body on). Runs inside ls_lsx_step, under the service lock. */
static bool party_probe(void *c, const ls_lsx_peer_t *p) { (void)c; (void)p; return false; }
static void party_dgram(void *c, uint32_t peer, uint8_t port, const uint8_t *d, int n)
{
    (void)c;
    if (!S.party_rx || !S.x) return;
    ls_lsx_status_t st;
    ls_lsx_status(S.x, &st);
    S.party_rx(S.party_ctx, peer, port, d, n, st.rssi, st.snr);
}

static const ls_lsx_store_t *store_now(void)
{
    if (S.store_test) return S.store_test;
    if (S.store_ext) return S.store_ext;
    return ls_lsx_default_store();
}

static int parse_hex16(const char *s, uint8_t out[16])
{
    if (strlen(s) != 32) return -1;
    for (int i = 0; i < 16; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* A key's fingerprint: four bytes of CMAC over a fixed label, enough to see
   that two boards hold the same key without showing it. */
static uint32_t key_print(const uint8_t key[16])
{
    uint8_t f[16];
    lsx_kdf(key, "LSX fingerprint", NULL, 0, f);
    return ((uint32_t)f[0] << 24) | ((uint32_t)f[1] << 16) | ((uint32_t)f[2] << 8) | f[3];
}

#ifdef ESP_PLATFORM
/* ======================================================================== */
#include "esp_console.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ls_field.h"
#include "ls_lora.h"
#include "ls_mesh.h"
#include "lr_iq_spectrum.h"
#include "nvs.h"

#define NVS_NS "lsx"

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_stop;
/* Each task runs for one generation: a stop that gave up waiting moves the
   generation on, so a task still busy in the store cannot carry on beside
   the next one (or clear the next one's handle) once it gets free. */
static volatile uint32_t s_gen;
static uint8_t *s_blob;
static esp_log_level_t s_log_lr = ESP_LOG_INFO, s_log_sx = ESP_LOG_INFO;

static void lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }
/* For the screens: the link task holds the lock through a whole step, and a
   step can wait on the atlas's card IO. A screen does not wait that out. */
static bool lock_try(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(15)) == pdTRUE;
}
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t rand32(void) { return esp_random(); }

/* ---- settings ----------------------------------------------------------- */

static void load(void)
{
    if (S.loaded) return;
    S.loaded = true;
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    S.id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    if (!S.id || S.id == LSX_BCAST) S.id = 0x4C530001u;
    snprintf(S.name, sizeof(S.name), "LS-%04X", (unsigned)(S.id & 0xFFFF));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = 16;
    S.key_set = nvs_get_blob(h, "key", S.key, &n) == ESP_OK && n == 16;
    uint8_t v;
    int8_t p;
    if (nvs_get_u8(h, "region", &v) == ESP_OK && v < LSX_REGION_N) S.region = v;
    if (nvs_get_u8(h, "maxmode", &v) == ESP_OK && v < LSX_M_N) S.max_mode = v;
    if (nvs_get_i8(h, "power", &p) == ESP_OK) S.power = p;
    if (nvs_get_u8(h, "auto", &v) == ESP_OK) S.autorole = v;
    n = sizeof(S.name);
    char nm[LSX_NAME];
    if (nvs_get_str(h, "name", nm, &n) == ESP_OK && nm[0]) memcpy(S.name, nm, sizeof(S.name));
    nvs_close(h);
}

static void save_blob(const char *k, const void *d, size_t n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, k, d, n);
    nvs_commit(h);
    nvs_close(h);
}

static void save_u8(const char *k, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, k, v);
    nvs_commit(h);
    nvs_close(h);
}

static void save_power(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, "power", S.power);
    nvs_commit(h);
    nvs_close(h);
}

static void save_name(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "name", S.name);
    nvs_commit(h);
    nvs_close(h);
}

static void load_units(ls_lsx_sync_t *sync)
{
    nvs_handle_t h;
    if (!s_blob) s_blob = malloc(1024);
    if (!s_blob || nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = 1024;
    if (nvs_get_blob(h, "units", s_blob, &n) == ESP_OK) ls_lsx_sync_units_load(sync, s_blob, n);
    nvs_close(h);
}

/* ---- the LoRa chip as the engine's phy ------------------------------------ */

static struct {
    bool asked, lease, fsk;
} P;

/* A LoRa chip that answered this boot, bound now or parked. A park (an app
   or MeshCore letting go of the chip, this service ending its lease) forgets
   the bound part, and the next configure or FSK start brings it back:
   ls_lora_present() alone read a parked chip as no chip at all, so PARTY
   said "no LoRa chip answered" until something else restarted it. */
static bool lora_fitted(void)
{
    return ls_lora_present() || (ls_lora_fitted_caps() & LS_LORA_CAP_LORA) != 0;
}

static uint32_t lora_caps_now(void)
{
    return ls_lora_present() ? ls_lora_caps() : ls_lora_fitted_caps();
}

static bool chip_busy_elsewhere(void)
{
    return !lora_fitted() || ls_field_owned() || ls_lora_scanning() || lr_spec_center();
}

static void park(void)
{
    if (P.fsk) ls_lora_fsk_end();
    else if (P.lease) ls_lora_park();
    P.fsk = false;
}

static bool p_claim(void *c, bool on)
{
    (void)c;
    if (!on) {
        if (P.lease) park();
        if (P.asked) ls_mesh_radio_hold_svc(false);
        P.asked = P.lease = false;
        return false;
    }
    if (!P.lease && chip_busy_elsewhere()) return false;
    P.asked = true;
    P.lease = ls_mesh_radio_hold_svc(true);
    return P.lease;
}

static bool p_yield(void *c)
{
    (void)c;
    return P.lease && ls_mesh_radio_svc_yield();
}

static int p_tune(void *c, int mode, uint32_t hz, int8_t dbm)
{
    (void)c;
    if (!P.lease) return -1;
    const ls_lsx_mode_info_t *m = ls_lsx_mode_info(mode);
    if (P.fsk) { ls_lora_fsk_end(); P.fsk = false; }
    if (m->kind == LSX_K_LORA) {
        ls_lora_cfg_t l;
        ls_lora_cfg_default(&l);
        l.freq_hz = hz;
        l.sf = 7;
        l.bw_hz = 125000;
        l.cr = 5;
        l.power_dbm = dbm;
        l.preamble = 8;
        l.sync_word = 0x12;
        l.crc_on = true;
        l.invert_iq = false;
        l.implicit_rx_bytes = 0;
        l.cal_min_mhz = (uint16_t)(hz / 1000000u);
        l.cal_max_mhz = (uint16_t)(hz / 1000000u + 1);
        if (ls_lora_configure(&l) != ESP_OK) return -1;
        return ls_lora_receive() == ESP_OK ? 0 : -1;
    }
    ls_fsk_cfg_t f = { 0 };
    f.freq_hz = hz;
    f.bitrate = m->kind == LSX_K_FLRC ? 250000 : m->bitrate;     /* FLRC: not used */
    f.deviation_hz = m->kind == LSX_K_FLRC ? 62500 : m->deviation_hz;
    f.bandwidth_hz = ls_lora_fsk_bw_snap(m->kind == LSX_K_FLRC ? 400000 : m->bandwidth_hz);
    f.sync_word = 0x7E5A3CC9u;
    f.payload_bytes = LSX_FRAME_MAX;
    f.preamble_bits = 32;
    f.power_dbm = dbm;
    f.sync_bits = 32;
    f.flrc = m->flrc;
    f.flrc_cr = m->flrc_cr;
    if (ls_lora_fsk_begin(&f) != ESP_OK) return -1;
    P.fsk = true;
    return 0;
}

static int p_send(void *c, const uint8_t *f, int len)
{
    (void)c;
    const esp_err_t e = P.fsk ? ls_lora_fsk_send(f, (size_t)len) : ls_lora_send(f, (size_t)len);
    return e == ESP_OK ? 0 : -1;
}

static int p_tx_done(void *c)
{
    (void)c;
    if (!ls_lora_send_done()) return 0;
    const esp_err_t e = P.fsk ? ls_lora_fsk_receive() : ls_lora_receive();
    return e == ESP_OK ? 1 : -1;
}

static int8_t clamp8(float v) { return (int8_t)(v != v ? -128 : v < -128 ? -128 : v > 127 ? 127 : v); }

static int p_recv(void *c, uint8_t *buf, int cap, ls_lsx_rxmeta_t *m)
{
    (void)c;
    float rssi = -128, snr = 127;
    const int n = P.fsk ? ls_lora_fsk_poll(buf, (size_t)cap, &rssi) : ls_lora_poll(buf, (size_t)cap, &rssi, &snr);
    if (n > 0 && m) {
        m->rssi = clamp8(rssi);
        m->snr = P.fsk ? 127 : clamp8(snr);
    }
    return n;
}

static ls_lsx_phy_t s_phy = { NULL, 0, p_claim, p_yield, p_tune, p_send, p_tx_done, p_recv };

/* ---- the task --------------------------------------------------------------- */

static void task(void *arg)
{
    const uint32_t gen = (uint32_t)(uintptr_t)arg;
    while (!s_stop && gen == s_gen) {
        lock();
        const uint32_t now = now_ms();
        uint32_t wait = ls_lsx_step(S.x, now);
        if (S.sync) ls_lsx_sync_tick(S.sync, now);
        size_t bl = 0;
        if (S.sync && ls_lsx_sync_take_dirty(S.sync) && S.role == LSX_ROLE_HOME && s_blob)
            bl = ls_lsx_sync_units_blob(S.sync, s_blob, 1024);
        unlock();
        if (bl) save_blob("units", s_blob, bl);
        if (wait > 20) wait = 20;
        vTaskDelay(wait ? pdMS_TO_TICKS(wait) : 1);
    }
    if (gen == s_gen) s_task = NULL;
    vTaskDelete(NULL);
}

static void stop_locked_wait(void)
{
    if (!s_task) return;
    s_stop = true;
    for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    s_gen++;            /* still running: it stops at its next check, whatever s_stop says by then */
    s_task = NULL;
}

void ls_lsx_svc_stop(void)
{
    stop_locked_wait();
    lock();
    ls_lsx_destroy(S.x);
    ls_lsx_sync_destroy(S.sync);
    S.x = NULL;
    S.sync = NULL;
    S.party_rx = NULL;
    S.role = LSX_ROLE_OFF;
    S.why = "stopped";
    unlock();
    esp_log_level_set("ls_lora_lr", s_log_lr);
    esp_log_level_set("ls_lora", s_log_sx);
}

bool ls_lsx_svc_start(int role)
{
    load();
    if (role != LSX_ROLE_HOME && role != LSX_ROLE_FIELD) { S.why = "no such role"; return false; }
    /* PARTY holds the link: stopping it here would leave PARTY showing
       joined while every datagram it sends fails. */
    if (S.role == LSX_ROLE_PARTY) { S.why = "PARTY has the link: leave the party first"; return false; }
    if (S.role) ls_lsx_svc_stop();
    if (!S.key_set) { S.why = "no key: lsx key phrase <words> (the same on every board)"; return false; }
    if (!lora_fitted()) { S.why = "no LoRa chip answered"; return false; }
    /* Records pushed to a unit with nowhere to keep them would be marked
       delivered and lost: no store, no link. */
    if (!store_now()) { S.why = "no atlas yet: open WARDRIVE once (or 'lsx testdata <n>')"; return false; }
    s_phy.caps = ls_lsx_svc_caps();
    ls_lsx_sync_t *sync = ls_lsx_sync_create(role);
    if (!sync) { S.why = "no memory"; return false; }
    if (role == LSX_ROLE_HOME) load_units(sync);
    ls_lsx_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.role = (uint8_t)role;
    cfg.id = S.id;
    memcpy(cfg.name, S.name, LSX_NAME);
    memcpy(cfg.key, S.key, 16);
    cfg.region = S.region;
    cfg.power_dbm = S.power;
    cfg.max_mode = S.max_mode;
    cfg.probe_ms = 8000;
    cfg.phy = &s_phy;
    cfg.app = ls_lsx_sync_app(sync);
    cfg.rand32 = rand32;
    ls_lsx_t *x = ls_lsx_create(&cfg);
    if (!x) { ls_lsx_sync_destroy(sync); S.why = "no memory"; return false; }
    ls_lsx_sync_bind(sync, x);
    ls_lsx_sync_set_store(sync, store_now());
    ls_lsx_set_rx_drop(x, S.drop);
    lock();
    S.x = x;
    S.sync = sync;
    S.role = role;
    S.why = "running";
    unlock();
    /* The driver logs every FSK frame at INFO: thousands a sync. */
    s_log_lr = esp_log_level_get("ls_lora_lr");
    s_log_sx = esp_log_level_get("ls_lora");
    esp_log_level_set("ls_lora_lr", ESP_LOG_WARN);
    esp_log_level_set("ls_lora", ESP_LOG_WARN);
    s_stop = false;
    if (xTaskCreate(task, "lsx", 6144, (void *)(uintptr_t)s_gen, 5, &s_task) != pdPASS) {
        ls_lsx_svc_stop();
        S.why = "no memory for the task";
        return false;
    }
    return true;
}

#define WITH_LOCK(stmt) do { lock(); stmt; unlock(); } while (0)

uint32_t ls_lsx_svc_caps(void)
{
    if (!lora_fitted()) return 0;
    const uint32_t c = lora_caps_now();
    return LSX_CAP_LORA | ((c & LS_LORA_CAP_FSK) ? LSX_CAP_GFSK : 0) | ((c & LS_LORA_CAP_FLRC) ? LSX_CAP_FLRC : 0);
}

bool ls_lsx_svc_party(const uint8_t key[16], ls_lsx_party_rx_t rx, void *ctx)
{
    load();
    if (S.role) ls_lsx_svc_stop();
    if (!lora_fitted()) { S.why = "no LoRa chip answered"; return false; }
    s_phy.caps = ls_lsx_svc_caps();
    ls_lsx_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.role = LSX_ROLE_PARTY;
    cfg.id = S.id;
    memcpy(cfg.name, S.name, LSX_NAME);
    memcpy(cfg.key, key, 16);
    cfg.region = S.region;
    cfg.power_dbm = S.power;
    cfg.max_mode = S.max_mode;
    cfg.phy = &s_phy;
    cfg.app.on_probe = party_probe;
    cfg.app.on_dgram = party_dgram;
    cfg.rand32 = rand32;
    ls_lsx_t *x = ls_lsx_create(&cfg);
    if (!x) { S.why = "no memory"; return false; }
    lock();
    S.x = x;
    S.sync = NULL;
    S.party_rx = rx;
    S.party_ctx = ctx;
    S.role = LSX_ROLE_PARTY;
    S.why = "running";
    unlock();
    s_log_lr = esp_log_level_get("ls_lora_lr");
    s_log_sx = esp_log_level_get("ls_lora");
    esp_log_level_set("ls_lora_lr", ESP_LOG_WARN);
    esp_log_level_set("ls_lora", ESP_LOG_WARN);
    s_stop = false;
    if (xTaskCreate(task, "lsx", 6144, (void *)(uintptr_t)s_gen, 5, &s_task) != pdPASS) {
        ls_lsx_svc_stop();
        S.why = "no memory for the task";
        return false;
    }
    return true;
}

int ls_lsx_svc_dgram(uint8_t port, const uint8_t *data, int len)
{
    int r = -1;
    WITH_LOCK(if (S.x && S.role == LSX_ROLE_PARTY) r = ls_lsx_send_dgram(S.x, LSX_BCAST, port, data, len));
    return r;
}

/* 'lsx auto': once the LoRa chip has answered (it starts after the
   console), or give up after 20 s. */
static void auto_start(void *arg)
{
    (void)arg;
    for (int i = 0; i < 40 && !ls_lora_present(); i++) vTaskDelay(pdMS_TO_TICKS(500));
    if (!ls_lsx_svc_start(S.autorole)) ESP_LOGW("lsx", "auto start: %s", S.why);
    vTaskDelete(NULL);
}

#else
/* ======================================================================== */
/* The host: a home base and a field unit over the loopback, stepped 100 ms
   of simulated time at each look, so lssim's frames show a sync moving.
   LSSIM_LSX=sx (an SX1262 car: GFSK), weak (-100 dBm), lossy (one frame in
   eight lost), field (this board is the car). */

static struct {
    ls_lsx_loop_t *loop;
    ls_lsx_t *peer;
    ls_lsx_sync_t *peer_sync;
    const ls_lsx_store_t *peer_store, *my_store;
    uint32_t now, grow_at;
} H;

static void lock(void) {}
static void unlock(void) {}
static bool lock_try(void) { return true; }
static void load(void)
{
    if (S.loaded) return;
    S.loaded = true;
    S.id = 0x484F0001u;
    strcpy(S.name, "HOME-0001");
    lsx_phrase_key("lssim", S.key);
    S.key_set = true;
}
static void save_u8(const char *k, uint8_t v) { (void)k; (void)v; }
static void save_blob(const char *k, const void *d, size_t n) { (void)k; (void)d; (void)n; }
static void save_power(void) {}
static void save_name(void) {}

static void host_pump(void)
{
    /* LSSIM_LSX set: the simulated pair starts the first time anything looks. */
    static bool autostarted;
    if (!S.x && !autostarted && getenv("LSSIM_LSX")) {
        autostarted = true;
        ls_lsx_svc_start(LSX_ROLE_HOME);
    }
    if (!S.x) return;
    for (int i = 0; i < 100; i++) {
        H.now++;
        ls_lsx_loop_time(H.loop, H.now);
        ls_lsx_step(S.x, H.now);
        ls_lsx_sync_tick(S.sync, H.now);
        ls_lsx_step(H.peer, H.now);
        ls_lsx_sync_tick(H.peer_sync, H.now);
    }
    /* The car keeps finding things: 120 more records every 40 s. */
    if ((int32_t)(H.now - H.grow_at) >= 0) {
        H.grow_at = H.now + 40000;
        ls_lsx_test_store_add(S.role == LSX_ROLE_HOME ? H.peer_store : H.my_store, 120);
    }
}

void ls_lsx_svc_stop(void)
{
    ls_lsx_destroy(S.x);
    ls_lsx_sync_destroy(S.sync);
    ls_lsx_destroy(H.peer);
    ls_lsx_sync_destroy(H.peer_sync);
    ls_lsx_loop_destroy(H.loop);
    S.x = H.peer = NULL;
    S.sync = H.peer_sync = NULL;
    H.loop = NULL;
    S.role = LSX_ROLE_OFF;
    S.why = "stopped";
}

/* PARTY on the host runs over its own loopback (party_link_loop.c). */
bool ls_lsx_svc_party(const uint8_t key[16], ls_lsx_party_rx_t rx, void *ctx)
{
    (void)key; (void)rx; (void)ctx; (void)party_probe; (void)party_dgram;
    S.why = "no LoRa chip on the host";
    return false;
}
int ls_lsx_svc_dgram(uint8_t port, const uint8_t *data, int len) { (void)port; (void)data; (void)len; return -1; }
uint32_t ls_lsx_svc_caps(void) { return 0; }

bool ls_lsx_svc_start(int role)
{
    load();
    if (S.role) ls_lsx_svc_stop();
    const char *env = getenv("LSSIM_LSX");
    const bool sx = env && strstr(env, "sx"), weak = env && strstr(env, "weak"), lossy = env && strstr(env, "lossy");
    if (env && strstr(env, "field")) role = LSX_ROLE_FIELD;
    const uint32_t lr = LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC;
    H.loop = ls_lsx_loop_create(lr, sx ? (LSX_CAP_LORA | LSX_CAP_GFSK) : lr, 77);
    ls_lsx_loop_set(H.loop, lossy ? 32 : 0, (int8_t)(weak ? -100 : -64));
    if (!H.my_store) H.my_store = ls_lsx_test_store(role == LSX_ROLE_HOME ? 400 : 4000, 1);
    if (!H.peer_store) H.peer_store = ls_lsx_test_store(role == LSX_ROLE_HOME ? 4000 : 400, 2);
    const int peer_role = role == LSX_ROLE_HOME ? LSX_ROLE_FIELD : LSX_ROLE_HOME;
    for (int end = 0; end < 2; end++) {
        const bool me = end == 0;
        ls_lsx_sync_t *sync = ls_lsx_sync_create(me ? role : peer_role);
        ls_lsx_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.role = (uint8_t)(me ? role : peer_role);
        cfg.id = me ? S.id : 0x43415231u;
        strcpy(cfg.name, me ? S.name : (cfg.role == LSX_ROLE_HOME ? "HOME-SIM" : (sx ? "CAR-SX" : "CAR-LR")));
        memcpy(cfg.key, S.key, 16);
        cfg.power_dbm = 14;
        cfg.max_mode = LSX_M_N - 1;
        cfg.probe_ms = 4000;
        cfg.phy = ls_lsx_loop_phy(H.loop, end);
        cfg.app = ls_lsx_sync_app(sync);
        ls_lsx_t *x = ls_lsx_create(&cfg);
        ls_lsx_sync_bind(sync, x);
        ls_lsx_sync_set_store(sync, me ? (S.store_test ? S.store_test : H.my_store) : H.peer_store);
        if (me) { S.x = x; S.sync = sync; }
        else { H.peer = x; H.peer_sync = sync; }
    }
    H.now = 1000;
    H.grow_at = 40000;
    S.role = role;
    S.why = "running (simulated)";
    return true;
}

#define WITH_LOCK(stmt) do { stmt; } while (0)
#endif

/* ======================================================================== */

int ls_lsx_svc_role(void) { return S.role; }
const char *ls_lsx_svc_why(void) { return S.why; }
bool ls_lsx_svc_has_key(void) { load(); return S.key_set; }
uint32_t ls_lsx_svc_id(void) { load(); return S.id; }
const char *ls_lsx_svc_name(void) { load(); return S.name; }

/* The last answers, for a frame drawn while the link task is busy. */
static ls_lsx_status_t s_link_last;
static ls_lsx_sync_status_t s_sync_last = { .progress_pct = -1, .last_home_ms = UINT32_MAX };
static ls_lsx_unit_t s_units_last[LSX_UNITS];
static int s_nunits_last;

void ls_lsx_svc_status(ls_lsx_status_t *link, ls_lsx_sync_status_t *sync)
{
#ifndef ESP_PLATFORM
    host_pump();
#endif
    if (!lock_try()) {
        if (link) *link = s_link_last;
        if (sync) *sync = s_sync_last;
        return;
    }
    ls_lsx_status(S.x, &s_link_last);
    if (S.sync) ls_lsx_sync_status(S.sync, &s_sync_last);
    else { memset(&s_sync_last, 0, sizeof(s_sync_last)); s_sync_last.progress_pct = -1; s_sync_last.last_home_ms = UINT32_MAX; }
    unlock();
    if (link) *link = s_link_last;
    if (sync) *sync = s_sync_last;
}

int ls_lsx_svc_units(ls_lsx_unit_t *out, int max)
{
    if (lock_try()) {
        s_nunits_last = S.sync ? ls_lsx_sync_units(S.sync, s_units_last, LSX_UNITS) : 0;
        unlock();
    }
    const int n = s_nunits_last < max ? s_nunits_last : max;
    if (n > 0) memcpy(out, s_units_last, sizeof(out[0]) * (size_t)n);
    return n > 0 ? n : 0;
}

void ls_lsx_svc_sync_now(void) { WITH_LOCK(if (S.sync) ls_lsx_sync_force(S.sync)); }

void ls_lsx_set_store(const ls_lsx_store_t *store)
{
    lock();
    S.store_ext = store;
    if (S.sync) ls_lsx_sync_set_store(S.sync, store_now());
    const bool gone = S.role && !store_now();
    unlock();
    if (gone) {
        ls_lsx_svc_stop();
        S.why = "stopped: the atlas went away";
    }
}

void ls_lsx_home_status(ls_lsx_home_status_t *out)
{
    ls_lsx_status_t l;
    ls_lsx_sync_status_t s;
    memset(out, 0, sizeof(*out));
    out->progress_pct = -1;
    out->last_sync_ago_s = UINT32_MAX;
    out->mode = "";
    if (S.role != LSX_ROLE_FIELD) return;
    ls_lsx_svc_status(&l, &s);
    out->on = true;
    out->in_range = s.last_home_ms < 120000;
    out->syncing = l.state == LSX_ST_SESSION;
    out->progress_pct = out->syncing ? s.progress_pct : -1;
    memcpy(out->home, s.home_name, LSX_NAME);
    out->rssi = l.rssi;
    out->mode = out->syncing ? ls_lsx_mode_info(l.mode)->name : "";
    out->recs_up = s.recs_out;
    out->recs_down = s.recs_in;
    if (s.last_sync_s) out->last_sync_ago_s = s.now_s >= s.last_sync_s ? s.now_s - s.last_sync_s : 0;
}

/* ---- 'lsx' ------------------------------------------------------------------ */

static void print_status(void)
{
    ls_lsx_status_t l;
    ls_lsx_sync_status_t s;
    ls_lsx_svc_status(&l, &s);
    static const char *const roles[] = { "off", "home", "field", "party" };
    static const char *const phases[] = { "idle", "pull", "push", "done" };
    printf("lsx: %s, %s; id %08lX \"%s\"; key %s", roles[S.role & 3], S.why, (unsigned long)S.id, S.name,
           S.key_set ? "set" : "NOT SET");
    if (S.key_set) printf(" (print %08lX)", (unsigned long)key_print(S.key));
    printf("\nlsx: region %s, power %d dBm, modes up to %s, store %s\n", ls_lsx_region(S.region)->name, S.power,
           ls_lsx_mode_info(S.max_mode)->name, s.store ? "bound" : "none");
    if (!S.role) return;
    printf("lsx: state %s%s, radio %s, %s %.3f MHz\n", ls_lsx_state_name(l.state), l.master ? " (master)" : "",
           l.held ? "held" : "free", ls_lsx_mode_info(l.mode)->name, l.hz / 1e6);
    if (l.state == LSX_ST_SESSION)
        printf("lsx: session with %08lX \"%s\" %lu ms, rssi %d snr %d, peer hears us %d, phase %s %d%%\n",
               (unsigned long)l.peer, l.peer_name, (unsigned long)l.session_ms, l.rssi, l.snr, l.rssi_at_peer,
               phases[s.phase & 3], s.progress_pct);
    printf("lsx: frames rx %lu tx %lu bad %lu, turns %lu missed %lu, bytes rx %lu tx %lu, sessions %lu resumed %lu,"
           " airtime %lu ms/60 s\n",
           (unsigned long)l.frames_rx, (unsigned long)l.frames_tx, (unsigned long)l.frames_bad, (unsigned long)l.turns,
           (unsigned long)l.misses, (unsigned long)l.bytes_rx, (unsigned long)l.bytes_tx, (unsigned long)l.sessions,
           (unsigned long)l.resumed, (unsigned long)l.airtime_ms_60s);
    printf("lsx: records in %lu out %lu", (unsigned long)s.recs_in, (unsigned long)s.recs_out);
    if (S.role == LSX_ROLE_FIELD)
        printf(", home \"%s\" heard %s%lu s ago, last sync %s", s.home_name, s.last_home_ms == UINT32_MAX ? "never " : "",
               (unsigned long)(s.last_home_ms == UINT32_MAX ? 0 : s.last_home_ms / 1000), s.last_sync_s ? "done" : "never");
    if (s.test_ms)
        printf(", last push %lu B in %lu ms = %lu kbit/s", (unsigned long)s.test_bytes, (unsigned long)s.test_ms,
               (unsigned long)(s.test_bytes * 8ull / (s.test_ms ? s.test_ms : 1)));
    printf("\n");
}

static void print_units(void)
{
    ls_lsx_unit_t u[LSX_UNITS];
    const int n = ls_lsx_svc_units(u, LSX_UNITS);
    if (!n) { printf("lsx: no units yet\n"); return; }
    for (int i = 0; i < n; i++)
        printf("lsx: unit %08lX \"%s\" syncs %u, have its seq %lu, gave it seq %lu, records in %lu out %lu, bytes in %lu"
               " out %lu, rssi %d\n",
               (unsigned long)u[i].id, u[i].name, (unsigned)u[i].syncs, (unsigned long)u[i].pulled,
               (unsigned long)u[i].pushed, (unsigned long)u[i].recs_in, (unsigned long)u[i].recs_out,
               (unsigned long)u[i].bytes_in, (unsigned long)u[i].bytes_out, u[i].rssi);
}

static int usage(void)
{
    printf("lsx                           status\n"
           "lsx home on|off               be the home base (listens, pulls, pushes)\n"
           "lsx field on|off              be a field unit (probes every 8 s)\n"
           "lsx sync                      field: probe now and sync even if up to date\n"
           "lsx key phrase <words...>     set the network key from a phrase (all boards the same)\n"
           "lsx key <32 hex digits>       set the network key\n"
           "lsx units                     home: what each unit has synced\n"
           "lsx push <bytes>              send test bytes to the session peer, timed\n"
           "lsx hold on|off               home: keep the session open after the sync (for push)\n"
           "lsx testdata <records>|off    a store of test records instead of the atlas\n"
           "lsx mode auto|<mode>          highest mode tried: LORA GFSK50 GFSK200 FLRC130 FLRC325 FLRC975\n"
           "lsx power <dBm>               transmit power (-9..22, region cap applies)\n"
           "lsx region us|eu              band plan\n"
           "lsx name <text>               this board's name on the link\n"
           "lsx drop <0..255>             test: lose that many of 256 received frames\n"
           "lsx auto home|field|off       start that role at boot\n"
           "lsx selftest                  AES and CMAC against published vectors\n");
    return 0;
}

int ls_lsx_command(int argc, char **argv)
{
    load();
    if (argc < 2) { print_status(); return 0; }
    const char *a = argv[1];
    const char *b = argc > 2 ? argv[2] : "";
    if (!strcmp(a, "help")) return usage();
    if (!strcmp(a, "home") || !strcmp(a, "field")) {
        const int role = !strcmp(a, "home") ? LSX_ROLE_HOME : LSX_ROLE_FIELD;
        if (!strcmp(b, "off")) {
            if (S.role == role) ls_lsx_svc_stop();
            printf("lsx: %s off\n", a);
            return 0;
        }
        if (!ls_lsx_svc_start(role)) { printf("lsx: not started: %s\n", S.why); return 1; }
        printf("lsx: %s on, %s\n", a, S.why);
        return 0;
    }
    if (!strcmp(a, "sync")) {
        if (S.role != LSX_ROLE_FIELD) { printf("lsx: sync is a field unit's (lsx field on)\n"); return 1; }
        ls_lsx_svc_sync_now();
        printf("lsx: probing now\n");
        return 0;
    }
    if (!strcmp(a, "key")) {
        uint8_t k[16];
        if (!strcmp(b, "phrase") && argc > 3) {
            char text[128] = "";
            for (int i = 3; i < argc; i++) {
                if (i > 3) strncat(text, " ", sizeof(text) - strlen(text) - 1);
                strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
            }
            lsx_phrase_key(text, k);
        } else if (parse_hex16(b, k) != 0) {
            printf("lsx: key phrase <words...> | key <32 hex digits>\n");
            return 1;
        }
        memcpy(S.key, k, 16);
        S.key_set = true;
        save_blob("key", k, 16);
        WITH_LOCK(if (S.x && S.role != LSX_ROLE_PARTY) ls_lsx_set_key(S.x, k));   /* PARTY keeps its party's key */
        printf("lsx: key set, print %08lX (compare it on the other boards)\n", (unsigned long)key_print(k));
        return 0;
    }
    if (!strcmp(a, "units")) { print_units(); return 0; }
    if (!strcmp(a, "hold")) {
        const bool on = strcmp(b, "off") != 0;
        WITH_LOCK(if (S.sync) ls_lsx_sync_hold(S.sync, on));
        printf("lsx: home %s the session open after a sync\n", on ? "keeps" : "closes");
        return 0;
    }
    if (!strcmp(a, "push")) {
        const long n = atol(b);
        int r = -1;
        WITH_LOCK(if (S.sync && n > 0) r = ls_lsx_sync_push_test(S.sync, (uint32_t)n));
        printf(r ? "lsx: no session, or a push is still going (1..32768 bytes)\n" : "lsx: pushing %ld bytes; 'lsx' shows the rate\n", n);
        return r ? 1 : 0;
    }
    if (!strcmp(a, "testdata")) {
        if (!strcmp(b, "off")) S.store_test = NULL;
        else {
            const long n = atol(b);
            S.store_test = ls_lsx_test_store(n > 0 ? (uint32_t)n : 1000, S.id);
        }
        WITH_LOCK(if (S.sync) ls_lsx_sync_set_store(S.sync, store_now()));
        printf("lsx: store %s\n", S.store_test ? "test records" : store_now() ? "the atlas" : "none");
        return 0;
    }
    if (!strcmp(a, "mode")) {
        int m = LSX_M_N - 1;
        for (int i = 0; i < LSX_M_N; i++) {
            const char *n = ls_lsx_mode_info(i)->name;
            int k = 0;
            while (n[k] && b[k] && (n[k] | 0x20) == (b[k] | 0x20)) k++;
            if (!n[k] && !b[k]) m = i;
        }
        S.max_mode = (uint8_t)m;
        save_u8("maxmode", S.max_mode);
        WITH_LOCK(if (S.x) ls_lsx_set_max_mode(S.x, m));
        printf("lsx: modes up to %s (next session)\n", ls_lsx_mode_info(m)->name);
        return 0;
    }
    if (!strcmp(a, "power")) {
        const int p = atoi(b);
        if (p < -9 || p > 22) { printf("lsx: power -9..22\n"); return 1; }
        S.power = (int8_t)p;
        save_power();
        WITH_LOCK(if (S.x) ls_lsx_set_power(S.x, S.power));
        printf("lsx: power %d dBm\n", p);
        return 0;
    }
    if (!strcmp(a, "region")) {
        S.region = !strcmp(b, "eu") ? LSX_REGION_EU868 : LSX_REGION_US915;
        save_u8("region", S.region);
        printf("lsx: region %s (restart the role to use it)\n", ls_lsx_region(S.region)->name);
        return 0;
    }
    if (!strcmp(a, "name") && b[0]) {
        snprintf(S.name, sizeof(S.name), "%s", b);
        save_name();
        printf("lsx: name \"%s\" (restart the role to use it)\n", S.name);
        return 0;
    }
    if (!strcmp(a, "drop")) {
        S.drop = (uint8_t)atoi(b);
        WITH_LOCK(if (S.x) ls_lsx_set_rx_drop(S.x, S.drop));
        printf("lsx: dropping %u of 256 received frames\n", (unsigned)S.drop);
        return 0;
    }
    if (!strcmp(a, "auto")) {
        S.autorole = !strcmp(b, "home") ? LSX_ROLE_HOME : !strcmp(b, "field") ? LSX_ROLE_FIELD : LSX_ROLE_OFF;
        save_u8("auto", S.autorole);
        printf("lsx: at boot: %s\n", S.autorole == LSX_ROLE_HOME ? "home" : S.autorole == LSX_ROLE_FIELD ? "field" : "nothing");
        return 0;
    }
    if (!strcmp(a, "selftest")) {
        const bool ok = lsx_crypto_selftest();
        printf("lsx: AES-128, CMAC and CRC-32 %s\n", ok ? "match the published vectors" : "FAIL");
        return ok ? 0 : 1;
    }
    return usage();
}

void ls_lsx_console_register(void)
{
#ifdef ESP_PLATFORM
    static const esp_console_cmd_t cmd = {
        .command = "lsx",
        .help = "LSX link: home base and field unit sync over LoRa/GFSK/FLRC (lsx help)",
        .hint = "[home|field on|off | sync | key phrase <w> | units | push <n> | mode <m> | help]",
        .func = &ls_lsx_command,
    };
    esp_console_cmd_register(&cmd);
    load();
    if (S.autorole) xTaskCreate(auto_start, "lsx_auto", 3072, NULL, 3, NULL);
#endif
}
