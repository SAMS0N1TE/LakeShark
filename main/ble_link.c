#include "ble_link.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "os/os_mbuf.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "nimble/nimble_opt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "store/config/ble_store_config.h"

#include "esp_hosted_misc.h"
#include "flipper_link.h"

static const char *TAG = "ble_link";

#define BLE_TX_ALLOC_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)

#define BLE_TX_HDR_BYTES 16

#define BLE_TX_ALIGN_SLACK 64

#define BLE_TX_DMA_MARGIN 512

void ble_store_config_init(void);

static const ble_uuid128_t SVC_SERIAL = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4b,
    0x52, 0x41, 0x48, 0x53, 0x45, 0x4b, 0x41, 0x4c);

static const ble_uuid128_t CHR_RX = BLE_UUID128_INIT(
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4b,
    0x52, 0x41, 0x48, 0x53, 0x45, 0x4b, 0x41, 0x4c);

static const ble_uuid128_t CHR_TX = BLE_UUID128_INIT(
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4b,
    0x52, 0x41, 0x48, 0x53, 0x45, 0x4b, 0x41, 0x4c);

#define BLE_LINE_MAX  192
#define REPLY_MAX 256
#define TEL_MAX   512

#define NO_SVC_BACKOFF_S 8

/*LS-106*/
#define RECONNECT_BACKOFF_MS      1500
#define RECONNECT_BACKOFF_MAX_MS  20000

/*LS-102*/
#define BLE_MIN_PAYLOAD 20

/*LS-104*/
#define CONN_ITVL_MIN_UNITS  24
#define CONN_ITVL_MAX_UNITS  48
#define CONN_LATENCY         0
#define CONN_TIMEOUT_UNITS   400

/*LS-101*/
#define CMD_Q_DEPTH 8

typedef struct {
    char line[BLE_LINE_MAX];
} ble_cmd_t;

static volatile ble_link_state_t s_state = BLE_LINK_OFF;
static volatile bool s_run     = false;
static volatile bool s_verbose = false;

static uint16_t s_conn    = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_rx_hnd  = 0;
static uint16_t s_tx_hnd  = 0;
static uint16_t s_tx_cccd = 0;
 static bool     s_rx_no_rsp = true;
static bool     s_tx_indicate = false;

static char s_name_filter[24] = "Lr1cher";
static char s_peer_name[32]   = "";
static char s_peer_addr[20]   = "";
static ble_addr_t s_peer_id;

static uint32_t s_rx_lines = 0, s_tx_frames = 0, s_drops = 0;
static int      s_tel_hz   = 5;

static volatile bool s_tel_allowed = false;

static volatile bool     s_pk_wait = false;
static uint16_t          s_pk_conn = BLE_HS_CONN_HANDLE_NONE;

static volatile bool     s_pk_early_valid = false;
static volatile uint32_t s_pk_early = 0;

static bool     s_pair_failed  = false;
static volatile int64_t s_rescan_at_us = 0;

static bool     s_disc_started = false;

static bool     s_stack_up     = false;

static char s_line[BLE_LINE_MAX];
static int  s_line_pos = 0;

static TaskHandle_t s_tel_task = NULL;

/*LS-101*/
static QueueHandle_t s_cmd_q    = NULL;
static TaskHandle_t  s_cmd_task = NULL;

/*LS-103*/
static volatile uint16_t s_mtu = 0;

/*LS-106*/
static uint32_t s_backoff_ms = RECONNECT_BACKOFF_MS;

/*LS-111*/
static uint16_t s_svc_start = 0, s_svc_end = 0;
static uint32_t s_notify_foreign = 0;

/*LS-113*/
static uint32_t s_notify_rx = 0;
static uint32_t s_notify_bytes = 0;

static int  gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);

const char *ble_link_state_name(void)
{
    switch (s_state) {
    case BLE_LINK_OFF:         return "off";
    case BLE_LINK_SYNCING:     return "syncing";
    case BLE_LINK_SCANNING:    return "scanning";
    case BLE_LINK_CONNECTING:  return "connecting";
    case BLE_LINK_DISCOVERING: return "discovering";
    case BLE_LINK_READY:       return "ready";
    }
    return "?";
}

ble_link_state_t ble_link_state(void) { return s_state; }

void ble_link_set_name_filter(const char *s)
{
    if (s && *s) strlcpy(s_name_filter, s, sizeof(s_name_filter));
}

void ble_link_get_name_filter(char *out, size_t len)
{
    if (out) strlcpy(out, s_name_filter, len);
}

void ble_link_peer(char *name, size_t nl, char *addr, size_t al)
{
    if (name) strlcpy(name, s_peer_name, nl);
    if (addr) strlcpy(addr, s_peer_addr, al);
}

void ble_link_stats(uint32_t *rx, uint32_t *tx, uint32_t *drops)
{
    if (rx)    *rx    = s_rx_lines;
    if (tx)    *tx    = s_tx_frames;
    if (drops) *drops = s_drops;
}

/*LS-111*/
void ble_link_rx_debug(uint16_t *tx_hnd, uint16_t *svc_start, uint16_t *svc_end,
                       uint32_t *foreign)
{
    if (tx_hnd)    *tx_hnd    = s_tx_hnd;
    if (svc_start) *svc_start = s_svc_start;
    if (svc_end)   *svc_end   = s_svc_end;
    if (foreign)   *foreign   = s_notify_foreign;
}

/*LS-113*/
void ble_link_notify_stats(uint32_t *notifies, uint32_t *bytes)
{
    if (notifies) *notifies = s_notify_rx;
    if (bytes)    *bytes    = s_notify_bytes;
}

void ble_link_set_tel_hz(int hz)
{
    if (hz < 0)  hz = 0;
    if (hz > 20) hz = 20;
    s_tel_hz = hz;
}
int  ble_link_tel_hz(void)          { return s_tel_hz; }
void ble_link_set_verbose(bool en)  { s_verbose = en; }

void ble_link_allow_telemetry(bool allow)
{
    if (s_tel_allowed != allow) {
        ESP_LOGI(TAG, "telemetry %s", allow ? "enabled" : "held off");
    }
    s_tel_allowed = allow;
}

/*LS-102*/
static int ble_payload_cap(uint16_t conn)
{
    uint16_t mtu = s_mtu;
    if (!mtu) mtu = ble_att_mtu(conn);
    int cap = (int)mtu - 3;
    if (cap < BLE_MIN_PAYLOAD) cap = BLE_MIN_PAYLOAD;
    return cap;
}

/*LS-105*/
static bool ble_write(const char *data, int len)
{
    uint16_t conn = s_conn;
    uint16_t hnd  = s_rx_hnd;

    if (s_state != BLE_LINK_READY || !hnd || conn == BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }
    if (len <= 0) return true;

    if (os_msys_num_free() < 4) {
        s_drops++;
        if (s_drops < 5 || s_verbose) {
            ESP_LOGW(TAG, "mbuf pool low (%d free) - dropping a frame rather "
                          "than risking the NimBLE tx assert", os_msys_num_free());
        }
        return false;
    }

    int cap = ble_payload_cap(conn);

    size_t largest = heap_caps_get_largest_free_block(BLE_TX_ALLOC_CAPS);
    size_t chunk   = (size_t)(len < cap ? len : cap);
    size_t need    = chunk + BLE_TX_HDR_BYTES + BLE_TX_ALIGN_SLACK +
                     BLE_TX_DMA_MARGIN;
    if (largest < need) {
        s_drops++;
        if (s_drops < 5 || s_verbose) {
            ESP_LOGW(TAG, "DMA-capable heap tight (largest block %u B, need %u) - "
                          "dropping a frame rather than panicking in the transport",
                     (unsigned)largest, (unsigned)need);
        }
        return false;
    }

    int sent = 0;
    while (sent < len) {
        int n = len - sent;
        if (n > cap) n = cap;

        int rc = s_rx_no_rsp
                     ? ble_gattc_write_no_rsp_flat(conn, hnd, data + sent, (uint16_t)n)
                     : ble_gattc_write_flat(conn, hnd, data + sent, (uint16_t)n,
                                            NULL, NULL);
        if (rc != 0) {
            s_drops++;
            if (s_verbose || s_drops < 5) {
                ESP_LOGW(TAG, "write failed rc=%d (%d of %d B out)", rc, sent, len);
            }
            /*LS-109*/
            if (sent > 0 && data[len - 1] == '\n') {
                ble_gattc_write_no_rsp_flat(conn, hnd, "\n", 1);
            }
            return false;
        }
        sent += n;

        if (sent < len && os_msys_num_free() < 4) {
            vTaskDelay(1);
        }
    }
    s_tx_frames++;
    return true;
}

/*LS-101*/
static void cmd_task(void *arg)
{
    (void)arg;
    ble_cmd_t cmd;
    char      reply[REPLY_MAX];

    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (!s_run) continue;

        reply[0] = '\0';
        flipper_link_inject(cmd.line, reply, sizeof(reply));
        if (reply[0]) ble_write(reply, (int)strlen(reply));
    }
}

static void tel_task(void *arg)
{
    (void)arg;
    char tel[TEL_MAX];
    for (;;) {
        if (!s_run) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        int64_t due = s_rescan_at_us;
        if (due && esp_timer_get_time() >= due) {
            s_rescan_at_us = 0;
            if (s_state != BLE_LINK_READY) start_scan();
        }

        /*LS-107*/
        int hz = s_tel_hz;
        if (hz <= 0 || s_state != BLE_LINK_READY || !s_tel_allowed) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int len = flipper_link_snapshot(tel, sizeof(tel));
        if (len > 0) {
            if (len > (int)sizeof(tel) - 1) len = (int)sizeof(tel) - 1;
            ble_write(tel, len);
        }
        char eqline[96];
        int eqn = flipper_link_eq_snapshot(eqline, sizeof(eqline));
        if (eqn > 0) ble_write(eqline, eqn);
        vTaskDelay(pdMS_TO_TICKS(1000 / hz));
    }
}

/*LS-101*/
static void feed_rx(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_line_pos > 0) {
                s_line[s_line_pos] = '\0';
                s_rx_lines++;
                if (s_verbose) ESP_LOGI(TAG, "RX <%s>", s_line);

                if (s_cmd_q) {
                    ble_cmd_t cmd;
                    strlcpy(cmd.line, s_line, sizeof(cmd.line));
                    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
                        s_drops++;
                        if (s_drops < 5 || s_verbose) {
                            ESP_LOGW(TAG, "command queue full - dropped <%s>", s_line);
                        }
                    }
                }
                s_line_pos = 0;
            }
        } else if (s_line_pos < BLE_LINE_MAX - 1) {
            s_line[s_line_pos++] = c;
        } else {
            s_line_pos = 0;
            s_drops++;
        }
    }
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err->status != 0) {
        ESP_LOGE(TAG, "subscribe failed status=%d", err->status);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    s_state = BLE_LINK_READY;
    s_backoff_ms = RECONNECT_BACKOFF_MS;
    ESP_LOGI(TAG, "link ready: %s [%s] - subscribed, telemetry at %d Hz",
             s_peer_name, s_peer_addr, s_tel_hz);

    /*LS-104*/
    {
        struct ble_gap_upd_params up = { 0 };
        up.itvl_min            = CONN_ITVL_MIN_UNITS;
        up.itvl_max            = CONN_ITVL_MAX_UNITS;
        up.latency             = CONN_LATENCY;
        up.supervision_timeout = CONN_TIMEOUT_UNITS;
        up.min_ce_len          = 0;
        up.max_ce_len          = 0;
        int urc = ble_gap_update_params(s_conn, &up);
        if (urc != 0 && urc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "conn param update rc=%d - staying on the peer's terms", urc);
        }
    }

    char hello[64];
    int n = snprintf(hello, sizeof(hello), "+HELLO %d LakeShark\n",
                     FLIPPER_LINK_PROTO_VERSION);
    ble_write(hello, n);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    (void)conn; (void)chr_val_handle; (void)arg;

    if (err->status == 0 && dsc &&
        ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s_tx_cccd = dsc->handle;
        ESP_LOGI(TAG, "CCCD for subscribe target at handle=%u", s_tx_cccd);
        return 0;
    }

    if (err->status == BLE_HS_EDONE) {
        if (!s_tx_cccd) {
            ESP_LOGE(TAG, "no CCCD on the TX characteristic");
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        uint8_t val[2] = { s_tx_indicate ? (uint8_t)0x02 : (uint8_t)0x01, 0x00 };
        ESP_LOGI(TAG, "enabling %s via CCCD %u",
                 s_tx_indicate ? "indications" : "notifications", s_tx_cccd);
        int rc = ble_gattc_write_flat(s_conn, s_tx_cccd, val, sizeof(val),
                                      on_cccd_written, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "cccd write rc=%d", rc);
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;

    if (err->status == 0 && chr) {

        bool ours = (ble_uuid_cmp(&chr->uuid.u, &CHR_RX.u) == 0) ||
                    (ble_uuid_cmp(&chr->uuid.u, &CHR_TX.u) == 0);
        if (!ours) return 0;

        if (chr->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) {
            s_tx_hnd = chr->val_handle;

            s_tx_indicate =
                (chr->properties & BLE_GATT_CHR_PROP_INDICATE) != 0;
            ESP_LOGI(TAG, "subscribe target: handle=%u props=0x%02x",
                     s_tx_hnd, chr->properties);
        }
        if (chr->properties & (BLE_GATT_CHR_PROP_WRITE |
                               BLE_GATT_CHR_PROP_WRITE_NO_RSP)) {
            s_rx_hnd = chr->val_handle;
            s_rx_no_rsp = (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
            ESP_LOGI(TAG, "write target: handle=%u props=0x%02x no_rsp=%d",
                     s_rx_hnd, chr->properties, s_rx_no_rsp);
        }
        return 0;
    }

    if (err->status == BLE_HS_EDONE) {
        if (!s_rx_hnd || !s_tx_hnd) {
            ESP_LOGE(TAG, "serial characteristics missing (rx=%u tx=%u) - is the "
                          "LakeShark app running on the head?", s_rx_hnd, s_tx_hnd);
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        int rc = ble_gattc_disc_all_dscs(conn, s_tx_hnd, s_tx_hnd + 2, on_dsc, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "disc dscs rc=%d", rc);
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;

    if (err->status == 0 && svc) {
        /*LS-111*/
        s_svc_start = svc->start_handle;
        s_svc_end   = svc->end_handle;
        ESP_LOGI(TAG, "serial service at handles %u..%u",
                 svc->start_handle, svc->end_handle);
        int rc = ble_gattc_disc_all_chrs(conn, svc->start_handle,
                                         svc->end_handle, on_chr, NULL);
        if (rc != 0) ESP_LOGE(TAG, "disc chrs rc=%d", rc);
        return 0;
    }

    if (err->status == BLE_HS_EDONE && !s_rx_hnd && !s_tx_hnd) {

        ESP_LOGE(TAG, "peer has no LakeShark service - is the head's app running "
                      "and set to BLE? backing off %d s", NO_SVC_BACKOFF_S);
        s_rescan_at_us = esp_timer_get_time() + NO_SVC_BACKOFF_S * 1000000LL;
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static const char *strcasestr_ci(const char *hay, const char *needle)
{
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++; n++;
        }
        if (!*n) return hay;
    }
    return NULL;
}

static bool adv_has_our_service(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids128; i++) {
        if (ble_uuid_cmp((const ble_uuid_t *)&f->uuids128[i], &SVC_SERIAL.u) == 0) {
            return true;
        }
    }
    return false;
}

static void adv_name_copy(const struct ble_hs_adv_fields *f, char *out, size_t len)
{
    int nl = f->name_len;
    if (!f->name || nl <= 0) { strlcpy(out, "(no name)", len); return; }
    char tmp[32];
    if (nl > (int)sizeof(tmp) - 1) nl = (int)sizeof(tmp) - 1;
    memcpy(tmp, f->name, nl);
    tmp[nl] = '\0';
    strlcpy(out, tmp, len);
}

static bool adv_name_matches(const struct ble_hs_adv_fields *f, char *out, size_t len)
{

    if (adv_has_our_service(f)) {
        adv_name_copy(f, out, len);
        return true;
    }

    const uint8_t *n = f->name;
    int nl = f->name_len;
    if (!n || nl <= 0) return false;

    char tmp[32];
    if (nl > (int)sizeof(tmp) - 1) nl = (int)sizeof(tmp) - 1;
    memcpy(tmp, n, nl);
    tmp[nl] = '\0';

    if (!strcasestr_ci(tmp, s_name_filter)) return false;
    strlcpy(out, tmp, len);
    return true;
}

static void addr_str(const ble_addr_t *a, char *out, size_t len)
{
    snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = { 0 };
    p.itvl          = 0;
    p.window        = 0;
    p.filter_policy = 0;
    p.limited       = 0;
    p.passive       = 0;
    p.filter_duplicates = 1;

    s_state = BLE_LINK_SCANNING;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
    else ESP_LOGI(TAG, "scanning for a head matching \"%s\"", s_name_filter);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, event->disc.data,
                                    event->disc.length_data) != 0) return 0;

        char name[32];
        bool matched = adv_name_matches(&f, name, sizeof(name));

        static int64_t s_last_adv_log_us = 0;
        static uint32_t s_adv_since = 0;
        s_adv_since++;

        int64_t now_us = esp_timer_get_time();
        bool due = s_verbose || matched || (now_us - s_last_adv_log_us) >= 1000000;
        if (due) {
            char seen[36] = "(no name)";
            if (f.name && f.name_len > 0) {
                int n = f.name_len < (int)sizeof(seen) - 1
                            ? f.name_len : (int)sizeof(seen) - 1;
                memcpy(seen, f.name, n);
                seen[n] = '\0';
            }
            char a[20];
            addr_str(&event->disc.addr, a, sizeof(a));
            ESP_LOGI(TAG, "adv: \"%s\" [%s] rssi=%d%s (%lu heard)", seen, a,
                     event->disc.rssi, matched ? " *MATCH*" : "",
                     (unsigned long)s_adv_since);
            s_last_adv_log_us = now_us;
            s_adv_since = 0;
        }
        if (!matched) return 0;

        s_peer_id = event->disc.addr;
        addr_str(&event->disc.addr, s_peer_addr, sizeof(s_peer_addr));
        strlcpy(s_peer_name, name, sizeof(s_peer_name));
        ESP_LOGI(TAG, "found \"%s\" [%s] rssi=%d - connecting",
                 s_peer_name, s_peer_addr, event->disc.rssi);

        ble_gap_disc_cancel();
        s_state = BLE_LINK_CONNECTING;
        int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr,
                                 10000, NULL, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "connect rc=%d", rc);
            start_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            /*LS-106*/
            ESP_LOGW(TAG, "connect failed status=%d - retrying in %lu ms",
                     event->connect.status, (unsigned long)s_backoff_ms);
            s_rescan_at_us = esp_timer_get_time() + (int64_t)s_backoff_ms * 1000;
            s_state        = BLE_LINK_SCANNING;
            s_backoff_ms *= 2;
            if (s_backoff_ms > RECONNECT_BACKOFF_MAX_MS) {
                s_backoff_ms = RECONNECT_BACKOFF_MAX_MS;
            }
            return 0;
        }
        s_conn    = event->connect.conn_handle;
        s_rx_hnd  = s_tx_hnd = s_tx_cccd = 0;
        s_line_pos = 0;
        s_pair_failed  = false;
        s_pk_wait      = false;
        s_disc_started = false;
        s_mtu          = 0;
        s_svc_start = s_svc_end = 0;
        s_state = BLE_LINK_DISCOVERING;

        /*LS-103*/
        {
            int mrc = ble_gattc_exchange_mtu(s_conn, NULL, NULL);
            if (mrc != 0 && mrc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "early exchange_mtu rc=%d", mrc);
            }
        }

        int src = ble_gap_security_initiate(s_conn);
        if (src != 0 && src != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "security_initiate rc=%d - trying discovery unpaired", src);
            s_disc_started = true;
            ble_gattc_disc_svc_by_uuid(s_conn, &SVC_SERIAL.u, on_svc, NULL);
        } else {
            ESP_LOGI(TAG, "connected, pairing...");
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason=%d)", event->disconnect.reason);
        s_conn   = BLE_HS_CONN_HANDLE_NONE;
        s_rx_hnd = s_tx_hnd = s_tx_cccd = 0;
        s_pk_wait      = false;
        s_mtu          = 0;
        s_svc_start = s_svc_end = 0;

        s_pk_early_valid = false;
        s_disc_started = false;

        if (event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL) ||
            event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)) {
            ESP_LOGW(TAG, "head rejected our stored keys - forgetting the bond, "
                          "expect a fresh passkey on the next attempt");
            ble_store_util_delete_peer(&s_peer_id);
            s_pair_failed = true;
        }

        if (!s_run) {
            s_state = BLE_LINK_OFF;
        } else if (s_pair_failed) {

            s_pair_failed  = false;
            s_rescan_at_us = esp_timer_get_time() + 5000000;
            s_state        = BLE_LINK_SCANNING;
            ESP_LOGW(TAG, "waiting 5 s before trying the head again");
        } else if (s_rescan_at_us) {

            s_state = BLE_LINK_SCANNING;
        } else {
            /*LS-106*/
            s_rescan_at_us = esp_timer_get_time() + (int64_t)s_backoff_ms * 1000;
            s_state        = BLE_LINK_SCANNING;
            ESP_LOGI(TAG, "rescanning in %lu ms", (unsigned long)s_backoff_ms);
            s_backoff_ms *= 2;
            if (s_backoff_ms > RECONNECT_BACKOFF_MAX_MS) {
                s_backoff_ms = RECONNECT_BACKOFF_MAX_MS;
            }
        }
        return 0;

    /*LS-111*/
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t h = event->notify_rx.attr_handle;
        if (h != s_tx_hnd) {
            s_notify_foreign++;
            static int64_t last_us = 0;
            int64_t now = esp_timer_get_time();
            if (s_verbose || s_notify_foreign <= 3 || now - last_us > 5000000LL) {
                last_us = now;
                ESP_LOGW(TAG, "notification on handle %u ignored (expected %u, "
                              "service %u..%u) - %lu so far", h, s_tx_hnd,
                         s_svc_start, s_svc_end,
                         (unsigned long)s_notify_foreign);
            }
            return 0;
        }
        /*LS-113*/
        s_notify_rx++;
        {
            int total = 0, segs = 0;
            for (struct os_mbuf *m = event->notify_rx.om; m; m = SLIST_NEXT(m, om_next)) {
                total += m->om_len;
                segs++;
            }
            s_notify_bytes += (uint32_t)total;
            if (s_verbose) {
                ESP_LOGI(TAG, "notify #%lu h=%u len=%d segs=%d",
                         (unsigned long)s_notify_rx, h, total, segs);
            }
        }

        struct os_mbuf *om = event->notify_rx.om;
        while (om) {
            feed_rx(om->om_data, om->om_len);
            om = SLIST_NEXT(om, om_next);
        }
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status != 0) {

            if (s_pair_failed) return 0;
            s_pair_failed = true;
            s_pk_wait     = false;
            ESP_LOGE(TAG, "pairing failed status=%d%s", event->enc_change.status,
                     event->enc_change.status == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_AUTHREQ)
                         ? " (head rejected our authentication requirements)"
                         : event->enc_change.status == BLE_HS_ETIMEOUT
                               ? " (nobody entered the passkey - see \"ble pin\")"
                               : "");

            if (event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) ||
                event->enc_change.status == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_ENC_KEY_SZ) ||
                event->enc_change.status == BLE_HS_SM_US_ERR(BLE_SM_ERR_ENC_KEY_SZ)) {
                ESP_LOGW(TAG, "the stored keys were rejected - forgetting the bond");
                ble_store_util_delete_peer(&s_peer_id);
            }

            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ESP_LOGI(TAG, "encryption change status=%d", event->enc_change.status);

        if (s_disc_started) {
            ESP_LOGD(TAG, "encryption re-reported - discovery already running");
            return 0;
        }
        s_disc_started = true;

        ESP_LOGI(TAG, "paired, discovering serial service");
        ble_gattc_disc_svc_by_uuid(s_conn, &SVC_SERIAL.u, on_svc, NULL);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:

        ESP_LOGW(TAG, "repeat pairing - dropping the old bond");
        {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &d) == 0) {
                ble_store_util_delete_peer(&d.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        switch (event->passkey.params.action) {

        case BLE_SM_IOACT_INPUT:

            s_pk_conn = event->passkey.conn_handle;
            s_pk_wait = true;

            if (s_pk_early_valid) {
                uint32_t code = s_pk_early;
                s_pk_early_valid = false;
                ESP_LOGI(TAG, "PAIRING: applying passkey %06lu received before "
                              "the request", (unsigned long)code);
                struct ble_sm_io io = { 0 };
                io.action  = BLE_SM_IOACT_INPUT;
                io.passkey = code;
                int rc = ble_sm_inject_io(s_pk_conn, &io);
                if (rc == 0) {
                    s_pk_wait = false;
                    return 0;
                }
                ESP_LOGW(TAG, "early passkey rejected (rc=%d), asking again", rc);
            }

            ESP_LOGW(TAG, "PAIRING: the head is showing a 6-digit code. "
                          "Type it here:  ble pin <code>");
            return 0;

        case BLE_SM_IOACT_NUMCMP:

            ESP_LOGI(TAG, "numeric comparison %06u - accepting",
                     (unsigned)event->passkey.params.numcmp);
            {
                struct ble_sm_io io = { 0 };
                io.action        = BLE_SM_IOACT_NUMCMP;
                io.numcmp_accept = 1;
                ble_sm_inject_io(event->passkey.conn_handle, &io);
            }
            return 0;

        default:
            ESP_LOGW(TAG, "passkey action %d is not something a headless "
                          "central can answer", event->passkey.params.action);
            return 0;
        }

    /*LS-103*/
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU now %d (payload cap %d B)", event->mtu.value,
                 ble_payload_cap(event->mtu.conn_handle));
        return 0;

    /*LS-104*/
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (event->conn_update.status == 0 &&
            ble_gap_conn_find(s_conn, &d) == 0) {
            ESP_LOGI(TAG, "conn params: itvl=%u latency=%u timeout=%u",
                     d.conn_itvl, d.conn_latency, d.supervision_timeout);
        } else if (event->conn_update.status != 0) {
            ESP_LOGW(TAG, "conn param update rejected status=%d",
                     event->conn_update.status);
        }
        return 0;
    }

    default:
        return 0;
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ESP_LOGI(TAG, "controller synced");
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "controller reset, reason=%d", reason);
    s_state = BLE_LINK_SYNCING;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/*LS-101*/
static bool workers_start(void)
{
    if (!s_cmd_q) {
        s_cmd_q = xQueueCreate(CMD_Q_DEPTH, sizeof(ble_cmd_t));
        if (!s_cmd_q) {
            ESP_LOGE(TAG, "command queue alloc failed");
            return false;
        }
    }
    if (!s_cmd_task &&
        xTaskCreate(cmd_task, "ble_cmd", 4608, NULL, 4, &s_cmd_task) != pdPASS) {
        ESP_LOGE(TAG, "command task create failed");
        return false;
    }
    if (!s_tel_task &&
        xTaskCreate(tel_task, "ble_tel", 6144, NULL, 4, &s_tel_task) != pdPASS) {
        ESP_LOGE(TAG, "telemetry task create failed");
        return false;
    }
    return true;
}

esp_err_t ble_link_start(void)
{
    if (s_run) return ESP_ERR_INVALID_STATE;

    if (s_stack_up) {
        s_run   = true;
        s_state = BLE_LINK_SCANNING;
        s_rescan_at_us = 0;
        s_backoff_ms   = RECONNECT_BACKOFF_MS;

        if (!workers_start()) {
            s_run = false;
            return ESP_ERR_NO_MEM;
        }
        start_scan();
        return ESP_OK;
    }

    /*LS-110*/
    esp_err_t rc = esp_hosted_bt_controller_init();
    ESP_LOGI(TAG, "co-processor bt_controller_init: %s", esp_err_to_name(rc));
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "co-processor refused to arm its BT controller. Its "
                      "ESP-Hosted slave firmware is too old to answer the "
                      "FeatureControl RPC - reflash the C6 (see c6_firmware/). "
                      "Leaving BLE off; the rest of the radio runs normally.");
        return rc;
    }

    rc = esp_hosted_bt_controller_enable();
    ESP_LOGI(TAG, "co-processor bt_controller_enable: %s", esp_err_to_name(rc));
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "co-processor BT controller would not enable - leaving BLE off");
        return rc;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_store_config_init();

    ESP_LOGW(TAG, "NIMBLE_BLE_SM=%d BLE_SM_LEGACY=%d BLE_SM_SC=%d",
             (int)NIMBLE_BLE_SM, (int)MYNEWT_VAL(BLE_SM_LEGACY),
             (int)MYNEWT_VAL(BLE_SM_SC));

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_hs_cfg.sm_io_cap        = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding       = 1;
    ble_hs_cfg.sm_mitm          = 0;
    ble_hs_cfg.sm_sc            = 1;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb  = ble_store_util_status_rr;

    /*LS-112*/
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("LakeShark");

    /*LS-103*/
    {
        int prc = ble_att_set_preferred_mtu(MYNEWT_VAL(BLE_ATT_PREFERRED_MTU));
        if (prc != 0) ESP_LOGW(TAG, "preferred MTU rc=%d", prc);
    }

    s_stack_up   = true;
    s_run        = true;
    s_state      = BLE_LINK_SYNCING;
    s_backoff_ms = RECONNECT_BACKOFF_MS;
    nimble_port_freertos_init(host_task);

    if (!workers_start()) {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ble_link_stop(void)
{
    if (!s_run) return;
    s_run = false;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_disc_cancel();
    s_state = BLE_LINK_OFF;
}

bool ble_link_passkey_pending(void) { return s_pk_wait; }

esp_err_t ble_link_submit_passkey(uint32_t code)
{
    if (code > 999999) return ESP_ERR_INVALID_ARG;

    if (!s_pk_wait || s_pk_conn == BLE_HS_CONN_HANDLE_NONE) {

        s_pk_early       = code;
        s_pk_early_valid = true;
        ESP_LOGI(TAG, "passkey %06lu held - the request has not arrived yet",
                 (unsigned long)code);
        return ESP_OK;
    }

    struct ble_sm_io io = { 0 };
    io.action  = BLE_SM_IOACT_INPUT;
    io.passkey = code;

    int rc = ble_sm_inject_io(s_pk_conn, &io);
    if (rc != 0) {
        ESP_LOGE(TAG, "inject_io rc=%d", rc);
        return ESP_FAIL;
    }
    s_pk_wait = false;
    ESP_LOGI(TAG, "passkey submitted, finishing pairing");
    return ESP_OK;
}

void ble_link_rescan(void)
{
    if (!s_run) return;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        ble_gap_disc_cancel();
        start_scan();
    }
}
