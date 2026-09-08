#include "ble_link.h"
#include "ls_board.h"   /*LS-981*/

#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
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
#include "host/ble_store.h"

#include "esp_hosted_misc.h"
#include "flipper_link.h"
/*LS-220*/
#include "ls_version.h"
/*LS-825*/
#include "ble_link_core.h"
#include "ble_link_host_task.h"
/*LS-993*/
#include "nvs.h"
#include "nvs_flash.h"
#include "ls_nvs_safe.h"

static const char *TAG = "ble_link";

#define BLE_TX_ALLOC_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)

#define BLE_TX_HDR_BYTES 16

#define BLE_TX_ALIGN_SLACK 64

/*LS-809  Was 512, which made this guard - not the heap - the thing that
   stopped telemetry. The floor for any write is MARGIN + header + slack +
   BLE_LINK_MIN_PAYLOAD, so 512 demanded 612 contiguous DMA bytes before a
   20-byte write was allowed. Measured on the LCD with the head connected:
   largest free block 512 B, so every frame was refused and the Flipper sat
   there printing NO SDR while the link reported itself ready and healthy.
   The real protection against NimBLE's tx assert is the mbuf check above
   (os_msys_num_free); this is only slack for the allocation itself, and
   128 B of it is ample for a write that is at most one MTU. */
#define BLE_TX_DMA_MARGIN 128

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

/*LS-813  The 16-bit id the head puts in its ADVERTISEMENT.

   SVC_SERIAL above is the 128-bit UUID of the GATT service, which is what
   the head SERVES - it is not what it advertises. A 128-bit UUID costs 18
   of the 31 bytes in an ADV_IND packet, so ls_ble_profile.c advertises the
   16-bit 0x18FF instead (`config->adv_service.Service_UUID_16`), exactly as
   ZeroMesh does. Change one and this must change with it. */
#define LS_HEAD_ADV_UUID16 0x18FF

/*LS-813  What the Flipper advertises when our app is NOT holding the radio:
   ble_profile_serial's 0x3080 | hw_colour, so 0x3080..0x3083. Recognised
   only so the log can name it. */
#define FLIPPER_STOCK_ADV_UUID16_BASE 0x3080
#define FLIPPER_STOCK_ADV_UUID16_MASK 0xfffc

/*LS-825  BLE_LINE_MAX, RECONNECT_BACKOFF_*, BLE_MIN_PAYLOAD and the
   CONN_ITVL/TIMEOUT figures moved to ble_link_core.h so the bench sees the
   same numbers this code does. */
#define BLE_LINE_MAX BLE_LINK_LINE_MAX

/*LS-511*/
#define REPLY_MAX 384
/*LS-518*/
#define TEL_MAX   576

#define NO_SVC_BACKOFF_S 8

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

/*LS-813  Empty by default, and an empty filter matches every head.

   This used to ship as "Lr1cher" - one developer's Flipper name compiled
   into the firmware, so nobody else's head could ever be found without an
   `ble name` first. The name is not what identifies a head anyway: the
   advertised service UUID is (see adv_has_our_service). The filter is only
   here to pick BETWEEN heads when more than one is on the air. */
static char s_name_filter[24] = "";
/*LS-813  When a Flipper on its stock profile was last heard, so `ble`
   can say why nothing is connected. 0 = never. */
static int64_t s_stock_head_seen_us = 0;
static char s_peer_name[32]   = "";
static char s_peer_addr[20]   = "";
static ble_addr_t s_peer_id;

/*LS-993  Pinned peer, so two boards do not race for one Flipper.  Loaded
   from NVS at ble_link_start(), rewritten only by ble_link_pin_peer() and
   ble_link_unpin_peer().  Passed to ble_link_scan_decide() on every adv. */
#define NVS_BLE_NS   "lakeshark"
#define NVS_BLE_PIN  "ble_pin"
static ble_link_peer_addr_t s_pinned = { .valid = 0 };

static uint32_t s_rx_lines = 0, s_tx_frames = 0, s_drops = 0;
static int      s_tel_hz   = 5;

static volatile bool s_tel_allowed = false;

static volatile bool     s_pk_wait = false;
static uint16_t          s_pk_conn = BLE_HS_CONN_HANDLE_NONE;

static volatile bool     s_pk_early_valid = false;
static volatile uint32_t s_pk_early = 0;

static bool     s_pair_failed  = false;
/*LS-714*/
static int      s_last_enc_status = 0;
static volatile int64_t s_rescan_at_us = 0;

static bool     s_disc_started = false;

static bool     s_stack_up     = false;

/*LS-825  Frame reassembly lives in ble_link_core so the bench can drive it. */
static ble_link_rx_t s_rx = { .pos = 0, .lines = 0, .drops = 0 };
static uint32_t      s_rx_drops_last = 0;

static TaskHandle_t s_tel_task = NULL;

/*LS-101*/
static QueueHandle_t s_cmd_q    = NULL;
static TaskHandle_t  s_cmd_task = NULL;

/*LS-103*/
static volatile uint16_t s_mtu = 0;

/*LS-106*/
static uint32_t s_backoff_ms = BLE_LINK_RECONNECT_BACKOFF_MS;

/*LS-111*/
static uint16_t s_svc_start = 0, s_svc_end = 0;
static uint32_t s_notify_foreign = 0;

/*LS-113*/
static uint32_t s_notify_rx = 0;
static uint32_t s_notify_bytes = 0;

static int  gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);
/*LS-980  LS-993 introduced pinned_load_nvs after ble_link_start(), and
   -Wimplicit-function-declaration is a hard error under IDF, so the p4-nano
   build refused it as soon as anything else in this file changed.  Forward
   declare here; the definition still lives with pinned_save_nvs. */
static void pinned_load_nvs(void);

const char *ble_link_state_name(void)
{
    return ble_link_state_name_of(s_state);
}

ble_link_state_t ble_link_state(void) { return s_state; }

void ble_link_set_name_filter(const char *s)
{
    if (s && *s) strlcpy(s_name_filter, s, sizeof(s_name_filter));
}

/*LS-813*/
bool ble_link_stock_head_seen(void)
{
    if (s_stock_head_seen_us == 0) return false;
    return (esp_timer_get_time() - s_stock_head_seen_us) < 120000000;
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
    return ble_link_payload_cap(mtu);
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

    /*LS-793  Fit the write to the heap instead of dropping the frame. The
       old guard compared one full-size write against the largest free block
       and gave up on the whole frame if it did not fit; with P25, the panel
       and USB all running that block sat near 768 B against an 845 B ask, so
       telemetry stopped dead while the link stayed up and the head showed
       nothing at all. A short write still carries the line - the loop below
       already chunks - so only give up when not even the minimum fits. */
    /*LS-810  A frame is all or nothing. Telemetry lines run past one MTU once
       append_sys() adds the uptime/heap suffix, so they always take several
       writes, and the head reassembles them by newline. Shortening a write to
       fit the heap (LS-793) meant a frame could start and then be refused part
       way through, and the head then saw a line beginning mid-field: it logged
       "rx junk", never parsed rtl=, and displayed NO SDR while this end
       reported the link ready and healthy. Decide once, for the whole frame,
       before sending any of it - the way it worked before LS-793 - and keep
       the smaller margin so frames actually fit. */
    size_t largest  = heap_caps_get_largest_free_block(BLE_TX_ALLOC_CAPS);
    size_t overhead = BLE_TX_HDR_BYTES + BLE_TX_ALIGN_SLACK + BLE_TX_DMA_MARGIN;
    int    fit      = ble_link_tx_chunk(largest, overhead,
                                        len < cap ? len : cap);
    if (fit <= 0) {
        s_drops++;
        if (s_drops < 5 || s_verbose) {
            ESP_LOGW(TAG, "DMA-capable heap tight (largest block %u B, need at "
                          "least %u) - dropping a frame rather than panicking "
                          "in the transport",
                     (unsigned)largest,
                     (unsigned)(overhead + BLE_LINK_MIN_PAYLOAD));
        }
        return false;
    }
    /*LS-810  Only shrink the write when the whole frame still gets out in
       chunks of that size; never emit a partial line. */
    if (fit < cap) {
        if (s_verbose) {
            ESP_LOGD(TAG, "DMA heap tight - writing %d B chunks instead of %d",
                     fit, cap);
        }
        cap = fit;
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

/*LS-101*/ /*LS-825*/
static void on_rx_line(const char *line, void *user)
{
    (void)user;
    s_rx_lines++;
    if (s_verbose) ESP_LOGI(TAG, "RX <%s>", line);

    if (s_cmd_q) {
        ble_cmd_t cmd;
        strlcpy(cmd.line, line, sizeof(cmd.line));
        if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
            s_drops++;
            if (s_drops < 5 || s_verbose) {
                ESP_LOGW(TAG, "command queue full - dropped <%s>", line);
            }
        }
    }
}

static void feed_rx(const uint8_t *data, int len)
{
    ble_link_rx_feed(&s_rx, data, len, on_rx_line, NULL);
    /* Overlong-line drops are counted inside the reassembler. Mirror the
       delta into s_drops so the transport-level counter still sees them. */
    uint32_t d = s_rx.drops - s_rx_drops_last;
    if (d) {
        s_drops        += d;
        s_rx_drops_last = s_rx.drops;
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
    /*LS-825*/
    s_state = ble_link_state_step(s_state, BLE_LINK_EV_SUBSCRIBED);
    s_backoff_ms = BLE_LINK_RECONNECT_BACKOFF_MS;
    ESP_LOGI(TAG, "link ready: %s [%s] - subscribed, telemetry at %d Hz",
             s_peer_name, s_peer_addr, s_tel_hz);

    /*LS-104*/ /*LS-824  The update is gone: it KILLED the link at 40 s.

       Once the link came up it lasted exactly 40 seconds, every time, and
       then:
           conn param update rejected status=546
           disconnected (reason=546)        (0x222 = LL response timeout)

       40 s is the Bluetooth link-layer response timeout. The head - a Flipper
       - never answers a connection-parameter update request, so the
       controller sat waiting the full timeout and then dropped the link. The
       "rejected" log is misleading: nothing rejected anything, the peer just
       never replied.

       It is also redundant now. LS-822 sets these same parameters in the
       ble_gap_connect() call, so the connection starts on our terms and there
       is nothing left to renegotiate. Asking again after the fact only gives
       a peer that cannot answer a chance to time the link out. */

    /*LS-220*/ /*LS-825*/
    /* HELLO on the BLE side too, so a Flipper reconnecting over BLE gets
       the firmware string without waiting for the first SYS reply. */
    char v[LS_VERSION_LINE_MAX];
    ls_version_line(v, sizeof(v));
    ble_link_sanitize_field(v);
    char hello[LS_VERSION_LINE_MAX + 32];
    int n = ble_link_format_hello(hello, sizeof(hello),
                                  FLIPPER_LINK_PROTO_VERSION, v);
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

/*LS-813  Look for the 16-bit id the head actually advertises.

   This only ever checked uuids128 for SVC_SERIAL, which the head does not
   advertise and cannot afford to - so has_our_service was false for every
   advertisement ever seen. Two failures came out of that:

     - Unpinned, ble_link_scan_decide fell through to the name filter alone.
       A Flipper advertises the same device name whichever BLE profile holds
       its radio, so when the LakeShark app was not running the P4 dialled
       the stock serial profile instead. That profile is bonding_mode=true
       with GapPairingPinCodeShow (MITM required, IO_CAP_DISPLAY_ONLY): it
       sends a Security Request the moment we connect, refuses our Just Works
       answer with SM error 3, and terminates. Forever, every 20 s:

           found "Lr1cher1" [80:e1:26:1c:5e:47] - connecting
           pairing failed status=1283 (head rejected our authentication
           requirements)
           connected - discovering service (no pairing required)
           disconnected (reason=517)          [0x205 = HCI auth failure]

       and the Flipper app - which never got a byte - sat there printing
       NO SDR. Every attempt to fix this by changing what WE offer (LS-811,
       LS-812) was aimed at the wrong peer: the stock profile will not do
       Just Works at all, so there is nothing to negotiate. The fix is to
       not dial it. Our own profile is ATTR_PERMISSION_NONE / GapPairingNone
       / bonding off and never asks for security, so once the filter is right
       the pairing path is simply never entered.

     - Pinned, the first line of ble_link_scan_decide requires the service,
       so `ble pin` bricked the link outright - nothing could ever match.

   ZeroMesh's node side has always filtered on this 16-bit id and has never
   had either problem. The 128-bit check is kept for a head that has room to
   advertise it. */
static bool adv_has_our_service(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids16; i++) {
        if (f->uuids16[i].value == LS_HEAD_ADV_UUID16) return true;
    }
    for (int i = 0; i < f->num_uuids128; i++) {
        if (ble_uuid_cmp((const ble_uuid_t *)&f->uuids128[i], &SVC_SERIAL.u) == 0) {
            return true;
        }
    }
    return false;
}

/*LS-813  True when the advertiser is a Flipper running its STOCK BLE
   profile rather than ours. Purely for the log: "found nothing" and "found
   your Flipper with the app closed" are different problems for the
   operator, and telling them apart on sight saves a bench session. */
static bool adv_is_flipper_stock(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids16; i++) {
        if ((f->uuids16[i].value & FLIPPER_STOCK_ADV_UUID16_MASK) ==
            FLIPPER_STOCK_ADV_UUID16_BASE) {
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

static bool adv_name_filter_hit(const struct ble_hs_adv_fields *f)
{
    const uint8_t *n = f->name;
    int nl = f->name_len;
    /*LS-813  No filter set means "any head", not "no head". The service
       UUID has already established that this IS one of ours by the time
       the name is consulted. */
    if (s_name_filter[0] == 0) return true;
    if (!n || nl <= 0) return false;

    char tmp[32];
    if (nl > (int)sizeof(tmp) - 1) nl = (int)sizeof(tmp) - 1;
    memcpy(tmp, n, nl);
    tmp[nl] = '\0';

    return strcasestr_ci(tmp, s_name_filter) != NULL;
}

/*LS-993  Look up the advertiser as a ble_link_peer_addr_t so the pure
   scan-decision helper (bench-tested) can be handed the raw bytes. */
static void adv_to_core_addr(const ble_addr_t *in, ble_link_peer_addr_t *out)
{
    memcpy(out->val, in->val, 6);
    out->type  = in->type;
    out->valid = 1;
}

static bool adv_name_matches(const struct ble_hs_adv_fields *f,
                             const ble_addr_t *addr,
                             char *out, size_t len)
{
    bool has_svc = adv_has_our_service(f);
    bool name_hit = adv_name_filter_hit(f);

    ble_link_peer_addr_t adv;
    adv_to_core_addr(addr, &adv);

    if (ble_link_scan_decide(has_svc, name_hit, &adv, &s_pinned) !=
        BLE_LINK_SCAN_CONNECT) {
        return false;
    }

    /* Copy the name for logging / display, matching pre-LS-993 behaviour. */
    if (has_svc) {
        adv_name_copy(f, out, len);
    } else {
        const uint8_t *n = f->name;
        int nl = f->name_len;
        char tmp[32];
        if (nl > (int)sizeof(tmp) - 1) nl = (int)sizeof(tmp) - 1;
        memcpy(tmp, n, nl);
        tmp[nl] = '\0';
        strlcpy(out, tmp, len);
    }
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

    /*LS-827  Report every advertisement, not the first one per address.

       Duplicate filtering makes the controller report each unique advertiser
       ONCE for the life of a scan, and this scan runs BLE_HS_FOREVER. That was
       harmless while we connected to the first head we saw on a name match:
       being told twice added nothing.

       LS-813 changed that. We now SKIP an advertiser that is not carrying our
       0x18FF service - a Flipper sitting on its stock BLE profile with the app
       closed - and a skipped address is then suppressed for the rest of the
       scan. The head can start advertising our service a second later and the
       controller will never mention it again.

       That is the "it stops working until I reopen the app" the operator saw.
       Reopening it is what makes it visible: ls_ble_profile does
       mac_address[2]++, so the app's advertisement carries a DIFFERENT address
       to the stock profile's, and a different address is a new advertiser the
       filter has not suppressed yet. The link came back for a reason that had
       nothing to do with the app restarting.

       The scan callback already rate-limits its own logging to one line a
       second, so the only thing filtering bought was hiding the head. */
    p.filter_duplicates = 0;

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
        bool matched = adv_name_matches(&f, &event->disc.addr, name, sizeof(name));

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
            /*LS-813  Print the advertised 16-bit service id. It is the one
               field that says which profile owns the head's radio, and not
               having it in the log is why the stock-profile connect went
               unnoticed for so long. */
            char svc[16] = "";
            if (f.num_uuids16 > 0) {
                snprintf(svc, sizeof(svc), " svc=0x%04x", f.uuids16[0].value);
            }
            ESP_LOGI(TAG, "adv: \"%s\" [%s]%s rssi=%d%s (%lu heard)", seen, a,
                     svc, event->disc.rssi, matched ? " *MATCH*" : "",
                     (unsigned long)s_adv_since);
            s_last_adv_log_us = now_us;
            s_adv_since = 0;
        }

        /*LS-813  Name the one failure an operator can actually fix.

           A Flipper on its stock BLE profile is not a head, and connecting
           to it is what produced the endless pairing loop. Say so, once a
           minute, instead of either dialling it or going silent - "no head
           found" and "your head is right there with the app closed" want
           very different things done about them. */
        if (!matched && adv_is_flipper_stock(&f)) {
            static int64_t s_last_stock_log_us = 0;
            s_stock_head_seen_us = now_us;
            if (now_us - s_last_stock_log_us >= 60000000) {
                s_last_stock_log_us = now_us;
                char a[20];
                addr_str(&event->disc.addr, a, sizeof(a));
                ESP_LOGW(TAG, "Flipper [%s] is advertising its own BLE profile, "
                              "not ours - open the LakeShark app on it", a);
            }
        }

        if (!matched) return 0;

        s_peer_id = event->disc.addr;
        addr_str(&event->disc.addr, s_peer_addr, sizeof(s_peer_addr));
        strlcpy(s_peer_name, name, sizeof(s_peer_name));
        ESP_LOGI(TAG, "found \"%s\" [%s] rssi=%d - connecting",
                 s_peer_name, s_peer_addr, event->disc.rssi);

        ble_gap_disc_cancel();
        /*LS-822  Connect with OUR parameters, not NimBLE's defaults.

           Passing NULL here takes the stack defaults, which carry
           supervision_timeout = 0x0100 = 256 units = 2.56 s. Every failed
           attempt against the Flipper died at exactly that mark:

             connected, pairing...
             MTU now 256 (payload cap 253 B)
             ...2.6 s later...
             pairing failed status=7   (BLE_HS_ENOTCONN)
             disconnected (reason=520) (0x208 = HCI supervision timeout)

           status=7 is the giveaway: pairing did not fail on its merits, the
           link was already gone when it was answered. CONN_TIMEOUT_UNITS
           (400 = 4 s) already existed for this link but was only applied in
           the connection-parameter UPDATE at BLE_GAP_EVENT_CONNECT - which
           never got the chance to run. Set it at connect time instead.

           scan_itvl/scan_window keep NimBLE's defaults; only the link
           timing is ours. */
        ble_link_conn_params_t cpc;
        ble_link_default_conn_params(&cpc);
        struct ble_gap_conn_params cp = {
            .scan_itvl           = cpc.scan_itvl,
            .scan_window         = cpc.scan_window,
            .itvl_min            = cpc.itvl_min,
            .itvl_max            = cpc.itvl_max,
            .latency             = cpc.latency,
            .supervision_timeout = cpc.supervision_timeout,
            .min_ce_len          = 0,
            .max_ce_len          = 0,
        };
        /*LS-825*/
        s_state = ble_link_state_step(s_state, BLE_LINK_EV_SCAN_MATCH);
        int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr,
                                 10000, &cp, gap_event, NULL);
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
            /*LS-825*/
            s_state       = ble_link_state_step(s_state, BLE_LINK_EV_CONNECT_FAIL);
            s_backoff_ms  = ble_link_backoff_next(s_backoff_ms);
            return 0;
        }
        s_conn    = event->connect.conn_handle;
        s_rx_hnd  = s_tx_hnd = s_tx_cccd = 0;
        ble_link_rx_reset(&s_rx);
        s_rx_drops_last = 0;
        s_pair_failed  = false;
        s_pk_wait      = false;
        s_disc_started = false;
        s_mtu          = 0;
        s_svc_start = s_svc_end = 0;
        /*LS-825*/
        s_state = ble_link_state_step(s_state, BLE_LINK_EV_CONNECT_OK);

        /*LS-103*/
        {
            int mrc = ble_gattc_exchange_mtu(s_conn, NULL, NULL);
            if (mrc != 0 && mrc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "early exchange_mtu rc=%d", mrc);
            }
        }

        /*LS-823  Discover FIRST. Do not make the link wait on pairing.

           The head is a Flipper, and a Flipper radio can only be a peripheral:
           it advertises a GATT service and waits to be dialled. That is the
           same shape ZeroMesh uses against a Meshtastic node, and its README
           is explicit about the security model - "No pairing or PIN."

           This code used to call ble_gap_security_initiate() and then sit in
           "connected, pairing..." until BLE_GAP_EVENT_ENC_CHANGE arrived. The
           old unpaired fallback only ran when security failed to START; when
           it started and the peer simply never answered, nothing moved and the
           link died on the supervision timer:

             connected, pairing...
             MTU now 256 (payload cap 253 B)
             pairing failed status=7      (BLE_HS_ENOTCONN - link already gone)
             disconnected (reason=520)    (0x208 = HCI supervision timeout)

           status=7 is the tell: pairing did not fail on its merits, it was
           answered after the connection had already dropped. Against a head
           that does not pair, initiating security is the bug.

           So: go straight to service discovery. Encryption is not required to
           read or subscribe on an unauthenticated characteristic, and
           BLE_GAP_EVENT_ENC_CHANGE is still handled if a head ever does bring
           security up on its own. A head that genuinely requires encryption
           will fail its first GATT op with an insufficient-authentication
           error, which is the point to escalate - not before. */
        s_disc_started = true;
        ESP_LOGI(TAG, "connected - discovering service (no pairing required)");
        {
            int drc = ble_gattc_disc_svc_by_uuid(s_conn, &SVC_SERIAL.u, on_svc, NULL);
            if (drc != 0) ESP_LOGW(TAG, "disc_svc_by_uuid rc=%d", drc);
        }
        /*LS-993  With no pin the scanner still accepts any matching device,
           so surface the address the operator would use to pin it - saves them
           reading it out of an earlier log line by hand. */
        if (!s_pinned.valid) {
            ESP_LOGI(TAG, "peer not pinned - `ble pin %s` locks this board to it",
                     s_peer_addr);
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

        /*LS-714*/ /*LS-825*/
        /* AUTH_FAIL covers two different failures. The head can reject the
           KEYS we stored, which is worth forgetting the bond over. Or it can
           refuse our security level outright (SM_ERR_AUTHREQ) - which is what
           a head with the app closed looks like, and has nothing to do with
           the keys. Deleting the bond on that second case destroyed a working
           bond every retry, forcing a fresh passkey on the next real connect.
           The classification itself lives in ble_link_core.c so the bench
           exercises the same decision this code does. */
        {
            ble_link_enc_kind_t last_kind = BLE_LINK_ENC_NONE;
            if (s_last_enc_status == 0) {
                last_kind = BLE_LINK_ENC_NONE;
            } else if (s_last_enc_status == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_AUTHREQ)) {
                last_kind = BLE_LINK_ENC_AUTHREQ_REFUSED;
            } else if (s_last_enc_status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) ||
                       s_last_enc_status == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_ENC_KEY_SZ) ||
                       s_last_enc_status == BLE_HS_SM_US_ERR(BLE_SM_ERR_ENC_KEY_SZ)) {
                last_kind = BLE_LINK_ENC_KEY_REJECTED;
            } else if (s_last_enc_status == BLE_HS_ETIMEOUT) {
                last_kind = BLE_LINK_ENC_TIMEOUT;
            } else {
                last_kind = BLE_LINK_ENC_OTHER_FAIL;
            }

            const bool auth_or_pinkey =
                (event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL) ||
                 event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING));
            switch (ble_link_classify_disc(auth_or_pinkey, last_kind)) {
            case BLE_LINK_DISC_AUTHREQ_REFUSAL:
                /*LS-811  Delete it, do not keep it. A head that answers with
                   AUTHREQ has no pairing method at all, so a bond for it can
                   never be used - but while one is stored NimBLE re-encrypts
                   at connection setup, the head refuses again and drops the
                   link. That is the loop seen on the bench: pairing fails
                   before discovery even starts, every connect, forever. The
                   enc_change path already wipes the bond for this reason; this
                   path contradicted it and kept the link permanently broken. */
                ESP_LOGW(TAG, "head refused our security level - forgetting the "
                              "bond so the next connect does not re-encrypt");
                ble_store_util_delete_peer(&s_peer_id);
                s_pair_failed = true;
                break;
            case BLE_LINK_DISC_KEY_REJECT:
                ESP_LOGW(TAG, "head rejected our stored keys - forgetting the bond, "
                              "expect a fresh passkey on the next attempt");
                ble_store_util_delete_peer(&s_peer_id);
                s_pair_failed = true;
                break;
            case BLE_LINK_DISC_UNRELATED:
                break;
            }
            s_last_enc_status = 0;
        }

        if (!s_run) {
            /*LS-825*/
            s_state = ble_link_state_step(s_state, BLE_LINK_EV_DISCONNECT_STOPPING);
        } else if (s_pair_failed) {

            /*LS-992  Back off, and stop narrating every attempt.

               A head that refuses our security refuses it every time, so a
               fixed 5 s retry is an infinite loop that emits about a dozen log
               lines per cycle. On a shared console that starves the REPL - a
               `wifi` command typed during it never echoed, let alone replied,
               and the board looked wedged when it was merely talking.

               So: the same backoff ladder every other failure uses, and the
               reason logged once per ladder rather than once per attempt. */
            s_pair_failed  = false;
            s_rescan_at_us = esp_timer_get_time() + (int64_t)s_backoff_ms * 1000;
            /*LS-825*/
            s_state        = ble_link_state_step(s_state, BLE_LINK_EV_DISCONNECT_RUNNING);
            if (s_backoff_ms == BLE_LINK_RECONNECT_BACKOFF_MS) {
                ESP_LOGW(TAG, "head refuses our security level - backing off "
                              "and retrying quietly (is the app open on it?)");
            }
            s_backoff_ms = ble_link_backoff_next(s_backoff_ms);
        } else if (s_rescan_at_us) {

            /*LS-825*/
            s_state = ble_link_state_step(s_state, BLE_LINK_EV_DISCONNECT_RUNNING);
        } else {
            /*LS-106*/
            s_rescan_at_us = esp_timer_get_time() + (int64_t)s_backoff_ms * 1000;
            /*LS-825*/
            s_state        = ble_link_state_step(s_state, BLE_LINK_EV_DISCONNECT_RUNNING);
            ESP_LOGI(TAG, "rescanning in %lu ms", (unsigned long)s_backoff_ms);
            s_backoff_ms = ble_link_backoff_next(s_backoff_ms);
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
            /*LS-714*/
            s_last_enc_status = event->enc_change.status;
            ESP_LOGE(TAG, "pairing failed status=%d%s", event->enc_change.status,
                     event->enc_change.status == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_AUTHREQ)
                         ? " (head rejected our authentication requirements)"
                         : event->enc_change.status == BLE_HS_ETIMEOUT
                               ? " (nobody entered the passkey - see \"ble passkey\")"
                               : "");

            /*LS-825*/
            ble_link_enc_kind_t k = BLE_LINK_ENC_OTHER_FAIL;
            if (event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) ||
                event->enc_change.status == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_ENC_KEY_SZ) ||
                event->enc_change.status == BLE_HS_SM_US_ERR(BLE_SM_ERR_ENC_KEY_SZ)) {
                k = BLE_LINK_ENC_KEY_REJECTED;
            } else if (event->enc_change.status == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_AUTHREQ)) {
                /*LS-980*/
                k = BLE_LINK_ENC_AUTHREQ_REFUSED;
            } else if (event->enc_change.status == BLE_HS_ETIMEOUT) {
                k = BLE_LINK_ENC_TIMEOUT;
            }
            if (ble_link_enc_kind_wipes_bond(k)) {
                ESP_LOGW(TAG, "stored security state is unusable with this head - forgetting it");
                ble_store_util_delete_peer(&s_peer_id);
            }

            /*LS-980  Keep the connection. Discovery has already finished (LS-823)
               and the characteristics need no encryption, so a link that failed
               to encrypt still carries telemetry and commands perfectly well.
               Terminating here is what made this a reconnect loop rather than a
               one-line warning. */
            if (!ble_link_enc_kind_needs_teardown(k)) {
                ESP_LOGW(TAG, "continuing unencrypted - this head does not pair");
                return 0;
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
                          "Type it here:  ble passkey <code>");
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
    ble_link_host_task_exit();
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
    /*LS-821  These two stacks MUST be in internal RAM.

       Reported as "connected my Flipper and it crashed", and the coredump
       named it exactly:

         Crashed task: 'ble_tel'
         assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
           cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())

       That assert fires when a task whose stack lives in PSRAM is on-CPU
       while the flash cache is disabled for a write. Pairing a head makes
       NimBLE persist the bond to NVS (BT_NIMBLE_NVS_PERSIST=y), which is a
       flash write, and these two tasks are the ones awake across it.

       Plain xTaskCreate() only lands the stack in PSRAM when internal RAM is
       short - which is precisely the GUI build on this board:
         heap after C6/BLE: internal=34023 DMA=679 largest-DMA=640
       so it is memory pressure that decides, and it will come and go with
       unrelated changes. SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y permits the
       spill; asking for MALLOC_CAP_INTERNAL here refuses it.

       Fixed at the two call sites rather than by clearing that sdkconfig
       option globally: on a build this tight, forcing every stack internal
       risks turning one crash into several task-create failures. Neither
       task is ever deleted, so plain WithCaps is safe without the matching
       vTaskDeleteWithCaps. */
    if (!s_cmd_task &&
        /*LS-114*/ /*LS-821*/
        /*LS-806  6144 with 5252 unused - about 892 B in use. These stacks have
           to be internal (LS-380), so their headroom costs the scarcest pool
           on the board. Trimmed to observed use plus ~2 KB. */
        xTaskCreateWithCaps(cmd_task, "ble_cmd", 3072, NULL, 4, &s_cmd_task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "command task create failed (internal RAM exhausted?)");
        return false;
    }
    if (!s_tel_task &&
        /*LS-821*/
        /*LS-806  6144 with 3680 unused - about 2464 B in use, so this one keeps
           more of its margin than ble_cmd. */
        xTaskCreateWithCaps(tel_task, "ble_tel", 4608, NULL, 4, &s_tel_task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "telemetry task create failed (internal RAM exhausted?)");
        return false;
    }
    return true;
}

esp_err_t ble_link_start(void)
{
    if (s_run) return ESP_ERR_INVALID_STATE;

    if (s_stack_up) {
        s_run   = true;
        /*LS-825*/
        s_state = ble_link_state_step(s_state, BLE_LINK_EV_START_UP);
        s_rescan_at_us = 0;
        s_backoff_ms   = BLE_LINK_RECONNECT_BACKOFF_MS;

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

    /*LS-980  Do not ask the head for security it does not offer.

       The Flipper profile sets pairing_method = GapPairingNone and marks both
       characteristics ATTR_PERMISSION_NONE, so nothing on that server requires
       encryption. Asking for bonding and Secure Connections against that is a
       requirements mismatch, and the head answers with SM error 3:

           pairing failed status=1283 (head rejected our authentication
           requirements)
           disconnected (reason=517)

       Observed on a nano and an LCD board independently, so it was never the
       two-board race it looked like.

       This link carries telemetry and commands over unencrypted, unauthenticated
       characteristics by the head's own design. Matching that is the honest
       posture, not a weakening of one - there was never a bond protecting
       anything here. */
    /*LS-813  Offer nothing, because nothing is needed.

       LS-812 tried the opposite - Secure Connections and bonding, on the
       theory that SM error 3 meant the head wanted MORE than we offered.
       It changed nothing, because the peer refusing us was never our head:
       it was the Flipper's stock serial profile, which requires MITM and a
       displayed PIN and will not do Just Works at any strength. See
       adv_has_our_service; the filter now keeps us away from it.

       Our head's own profile serves both characteristics at
       ATTR_PERMISSION_NONE and configures GapPairingNone with bonding off,
       so it never asks for security and there is nothing here to satisfy.
       Keeping sm_bonding at 0 also keeps NimBLE from writing bond records
       into NVS for a link that can never use them. */
    ble_hs_cfg.sm_io_cap        = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding       = 0;
    ble_hs_cfg.sm_mitm          = 0;
    ble_hs_cfg.sm_sc            = 0;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb  = ble_store_util_status_rr;

    /*LS-112*/
    ble_svc_gap_init();
    /*LS-981  Say which board this is. Two boards both advertising "LakeShark"
       are indistinguishable in a scanner, on the head, and in a bug report -
       which is exactly the confusion that made a pairing failure look like two
       boards fighting over one Flipper. */
    ble_svc_gap_device_name_set("LakeShark " LS_BOARD_NAME);

    /*LS-993  Read the pinned peer BEFORE the first scan starts so an operator
       who did `ble pin` in an earlier session is not racing again on this boot. */
    pinned_load_nvs();

    /*LS-103*/
    {
        int prc = ble_att_set_preferred_mtu(MYNEWT_VAL(BLE_ATT_PREFERRED_MTU));
        if (prc != 0) ESP_LOGW(TAG, "preferred MTU rc=%d", prc);
    }

    _Static_assert(BLE_LINK_HOST_STACK_BYTES == NIMBLE_HS_STACK_SIZE,
                   "NimBLE host stack storage must match Kconfig");

    s_stack_up   = true;
    s_run        = true;
    /*LS-825*/
    s_state      = ble_link_state_step(s_state, BLE_LINK_EV_START_COLD);
    s_backoff_ms = BLE_LINK_RECONNECT_BACKOFF_MS;
    /* LS-684: ble_hs_startup_go restores the NimBLE security database on this
       task. The matching e176126 coredump names nimble_host and shows the
       restore -> nvs_get -> esp_flash_read chain with SP=0x30101050 in P4 TCM.
       A fixed 5120-byte DRAM_ATTR stack is reserved by ble_link_host_task. */
    if (!ble_link_host_task_start(host_task, configMAX_PRIORITIES - 4,
                                  NIMBLE_CORE)) {
        s_stack_up = false;
        s_run = false;
        ESP_LOGE(TAG, "NimBLE host task create failed");
        return ESP_ERR_NO_MEM;
    }

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
    /*LS-825*/
    s_state = ble_link_state_step(s_state, BLE_LINK_EV_STOP);
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

/*LS-993  Pinned-peer persistence and API. */

/*LS-671  Same reason as pinned_load_nvs below: this can be reached from a
   task with an external stack (the console runs on one), and an NVS write
   disables the cache. */
static esp_err_t pinned_write_blob(void *ctx)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_BLE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (s_pinned.valid) {
        err = nvs_set_blob(h, NVS_BLE_PIN, &s_pinned, sizeof(s_pinned));
    } else {
        err = nvs_erase_key(h, NVS_BLE_PIN);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;  /* already absent */
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void pinned_save_nvs(void)
{
    esp_err_t err = ls_nvs_call(pinned_write_blob, NULL, 0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "pinned peer not saved: %s", esp_err_to_name(err));
}

/*LS-671  The flash access runs on an internal-stack worker.

   ble_link_start() is called from a task whose stack is in PSRAM, and
   CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY is enabled in this build, so
   reading NVS here disabled the cache under an unreachable stack and
   asserted at boot:

     assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
     cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())

   Only the NVS read moves. Deciding what to do with the result, and the
   log line, stay here on the caller. */
static esp_err_t pinned_read_blob(void *ctx)
{
    ble_link_peer_addr_t *out = (ble_link_peer_addr_t *)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_BLE_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(*out);
    err = nvs_get_blob(h, NVS_BLE_PIN, out, &len);
    if (err == ESP_OK && len != sizeof(*out)) err = ESP_ERR_INVALID_SIZE;
    nvs_close(h);
    return err;
}

static void pinned_load_nvs(void)
{
    ble_link_peer_addr_t p = { 0 };
    if (ls_nvs_call(pinned_read_blob, &p, 0) != ESP_OK) return;
    if (!p.valid) return;

    s_pinned = p;
    char pretty[24];
    ble_link_peer_addr_format(&s_pinned, pretty, sizeof(pretty));
    ESP_LOGI(TAG, "pinned peer restored from NVS: %s (type=%u)",
             pretty, (unsigned)s_pinned.type);
}

/* Parse "aa:bb:cc:dd:ee:ff" into an addr.  Big-endian on the wire, so val[0]
   is the last byte typed - matches addr_str() and ble_link_peer_addr_format
   both ways. */
static bool addr_parse(const char *s, ble_link_peer_addr_t *out)
{
    if (!s || !out) return false;
    unsigned b[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xff) return false;
        out->val[5 - i] = (uint8_t)b[i];
    }
    out->type  = 0;   /* public; NimBLE will match either way on discovery */
    out->valid = 1;
    return true;
}

bool ble_link_get_pinned(char *out_addr, size_t out_len)
{
    if (!s_pinned.valid) {
        if (out_addr && out_len) out_addr[0] = '\0';
        return false;
    }
    if (out_addr) ble_link_peer_addr_format(&s_pinned, out_addr, out_len);
    return true;
}

esp_err_t ble_link_pin_peer(const char *addr)
{
    ble_link_peer_addr_t p = { .valid = 0 };
    if (addr && *addr) {
        if (!addr_parse(addr, &p)) return ESP_ERR_INVALID_ARG;
    } else {
        /* Pin the currently-connected peer, if any. */
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) return ESP_ERR_NOT_FOUND;
        memcpy(p.val, s_peer_id.val, 6);
        p.type  = s_peer_id.type;
        p.valid = 1;
    }
    s_pinned = p;
    pinned_save_nvs();
    char pretty[24];
    ble_link_peer_addr_format(&s_pinned, pretty, sizeof(pretty));
    ESP_LOGW(TAG, "peer pinned: %s - other advertisers with our service are ignored",
             pretty);
    return ESP_OK;
}

void ble_link_unpin_peer(void)
{
    if (!s_pinned.valid) return;
    s_pinned.valid = 0;
    pinned_save_nvs();
    ESP_LOGW(TAG, "peer unpinned - the scanner will accept any matching device");
}

/*LS-980  Wipe every bond the store knows about and drop whatever link is up,
   so a stale bond from an earlier firmware cannot survive the upgrade.  The
   new config never initiates pairing (sm_bonding=0, sm_sc=0), so the
   ENC_CHANGE-driven wipe in gap_event() never runs on a fresh boot; without
   this hatch an operator would have to reflash to clear the store. */
esp_err_t ble_link_forget_bonds(void)
{
    int rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_store_clear rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "BLE bond store cleared - any stored pairing keys are gone");

    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return ESP_OK;
}

bool ble_link_is_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}
