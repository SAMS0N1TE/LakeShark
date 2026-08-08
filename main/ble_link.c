/* ble_link - flipper_link protocol over BLE. See header. */

#include "ble_link.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"       /* transport-heap backpressure - see ble_write() */

#include "os/os_mbuf.h"          /* os_msys_num_free() - tx backpressure */
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

/* What a BLE write really costs, mirrored from the allocation that fails.
 *
 * esp_hosted's vhci_drv.c does, for every outgoing ACL packet:
 *
 *     data_len = OS_MBUF_PKTLEN(om) + 1;
 *     data = _h_malloc_align(data_len, HOSTED_MEM_ALIGNMENT_64);
 *
 * and _h_malloc_align is heap_caps_aligned_alloc(64, size,
 * MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT). So the requirement is
 * one contiguous DMA-capable internal block of roughly the ATT payload plus the
 * ATT/L2CAP/ACL/H4 headers, rounded up for 64-byte alignment - a few hundred
 * bytes, not a few thousand.
 *
 * Getting this wrong in either direction is bad in a different way, and both
 * were observed: too small and the transport's malloc fails and NimBLE's hard
 * assert reboots the chip; too large and every frame is refused and the head
 * shows no radio at all. The caps below are the allocator's own, so the check
 * asks the same question the allocation will. */
#define BLE_TX_ALLOC_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
/* ATT opcode+handle (3) + L2CAP (4) + ACL (4) + H4 (1), rounded up. */
#define BLE_TX_HDR_BYTES 16
/* HOSTED_MEM_ALIGNMENT_64: an aligned allocation may need up to align-1 extra
 * to place the block. */
#define BLE_TX_ALIGN_SLACK 64
/* Margin for the window between this check and the allocation, during which
 * other tasks - the SDIO RX path especially - are allocating from the same
 * heap. */
#define BLE_TX_DMA_MARGIN 512

/* ESP-IDF's store/config/ble_store_config.h declares the three store callbacks
 * but not the initialiser that installs them. Every NimBLE example in the IDF
 * forward-declares it exactly like this. */
void ble_store_config_init(void);

/* The head's own LakeShark service, registered by ls_ble_profile.c in the FAP.
 * NimBLE stores 128-bit UUIDs least-significant byte first, so each of these is
 * the textual UUID reversed.
 *
 *   service 4c414b45-5348-4152-4b00-000000000001
 *   rx      4c414b45-5348-4152-4b00-000000000002   we write here  (-> Flipper)
 *   tx      4c414b45-5348-4152-4b00-000000000003   we subscribe   (<- Flipper)
 *
 * Moved off the Flipper's stock serial service (8fe5b3d5-...) because the head's
 * Bt service hijacks that one for RPC on every connect and feeds our ASCII to a
 * protobuf decoder, which then restarts the BLE core. A profile of our own is
 * never matched by that check. See HANDOFF.md.
 */
static const ble_uuid128_t SVC_SERIAL = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4b,
    0x52, 0x41, 0x48, 0x53, 0x45, 0x4b, 0x41, 0x4c);

static const ble_uuid128_t CHR_RX = BLE_UUID128_INIT(
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4b,
    0x52, 0x41, 0x48, 0x53, 0x45, 0x4b, 0x41, 0x4c);

static const ble_uuid128_t CHR_TX = BLE_UUID128_INIT(
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4b,
    0x52, 0x41, 0x48, 0x53, 0x45, 0x4b, 0x41, 0x4c);

#define LINE_MAX  192
#define REPLY_MAX 256
#define TEL_MAX   512

/* How long to wait after connecting to a head that turned out not to be serving
 * our profile, before trying it again. */
#define NO_SVC_BACKOFF_S 8

static volatile ble_link_state_t s_state = BLE_LINK_OFF;
static volatile bool s_run     = false;
static volatile bool s_verbose = false;

static uint16_t s_conn    = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_rx_hnd  = 0;   /* value handle we write to   */
static uint16_t s_tx_hnd  = 0;   /* value handle we listen on  */
static uint16_t s_tx_cccd = 0;
 static bool     s_rx_no_rsp = true;
static bool     s_tx_indicate = false;

static char s_name_filter[24] = "Lr1cher";
static char s_peer_name[32]   = "";
static char s_peer_addr[20]   = "";
static ble_addr_t s_peer_id;

static uint32_t s_rx_lines = 0, s_tx_frames = 0, s_drops = 0;
static int      s_tel_hz   = 5;
/* Starts closed: see ble_link_allow_telemetry(). app_main() opens it once the
 * radio pipeline has finished allocating. */
static volatile bool s_tel_allowed = false;

/* Passkey entry is inherently out-of-band for us: the code appears on the
 * head's screen and arrives here through the console, some seconds later. */
static volatile bool     s_pk_wait = false;
static uint16_t          s_pk_conn = BLE_HS_CONN_HANDLE_NONE;
/* A passkey that arrived before the SM asked for one; see the
 * BLE_SM_IOACT_INPUT case for why that happens with an automated relay. */
static volatile bool     s_pk_early_valid = false;
static volatile uint32_t s_pk_early = 0;

/* A failed pairing reports twice - once for the real SM error and once as
 * ENOTCONN from the teardown we started - and the reconnect is instant, so
 * without these the log is an unreadable loop hammering the radio. */
static bool     s_pair_failed  = false;
static volatile int64_t s_rescan_at_us = 0;

/* Encryption completes twice per connection (both ends initiate), so discovery
 * needs a latch of its own rather than keying off the handles it fills in. */
static bool     s_disc_started = false;

/* Host stack + co-processor controller are brought up once and left up. */
static bool     s_stack_up     = false;

/* Assembled from notification fragments; the Flipper may split a line. */
static char s_line[LINE_MAX];
static int  s_line_pos = 0;

static TaskHandle_t s_tel_task = NULL;

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

/* ------------------------------------------------------------------- tx */

/* The Flipper's serial characteristic accepts up to 243 bytes per write, but
 * only once the ATT MTU has actually been raised - the 23-byte default caps a
 * write at 20. We ask for a bigger MTU on every connection; this checks what we
 * were actually granted rather than assuming, because a silently truncated
 * frame loses its terminating newline and the head then never decodes a single
 * line. */
static bool ble_write(const char *data, int len)
{
    if (s_state != BLE_LINK_READY || !s_rx_hnd) return false;

    /* Leave headroom in the mbuf pool. NimBLE's ble_att_tx_with_conn() ends in
     *
     *     rc = ble_l2cap_tx(conn, chan, txom);
     *     assert(rc == 0);                      <- ble_att_cmd.c:91
     *
     * which is a hard assert, not a debug one. So an exhausted pool does not
     * fail the write and return an error we could count - it panics the entire
     * radio and reboots it. Telemetry at 5 Hz plus a command reply arriving at
     * the wrong moment is enough to get there, which is exactly what a key
     * press on the head used to do. Dropping a frame is always better. */
    if (os_msys_num_free() < 4) {
        s_drops++;
        if (s_drops < 5 || s_verbose) {
            ESP_LOGW(TAG, "mbuf pool low (%d free) - dropping a frame rather "
                          "than risking the NimBLE tx assert", os_msys_num_free());
        }
        return false;
    }

    /* The *other* way to reach that same assert, and the one the mbuf check
     * above cannot see.
     *
     * Below ble_l2cap_tx() the packet is handed to the esp_hosted VHCI
     * transport, which does its own plain malloc for the ACL buffer:
     *
     *     E vhci_drv: Tx ble_transport_to_ll_acl_impl: malloc failed
     *     assert failed: ble_att_tx_with_conn ble_att_cmd.c:91 (rc == 0)
     *
     * That allocation comes from the internal heap, not from NimBLE's mbuf
     * pool, so os_msys_num_free() can report plenty of room while the transport
     * cannot get a byte. Internal RAM is the scarce resource on this board and
     * it is at its tightest for a few seconds after boot, while the USB host
     * posts its IQ transfers and the DSD decoder takes its buffers - which is
     * exactly when a 5 Hz telemetry stream is also running. The result was a
     * reboot loop: crash, reconnect, crash again at the same point.
     *
     * Fragmentation matters more than the total here - the transport needs one
     * contiguous block - so this checks the largest free block rather than the
     * sum, and asks for real headroom above the frame rather than just enough.
     * Dropping telemetry frames while memory is tight is invisible to the head
     * (it re-renders from the next one); a panic is not.
     *
     * The caps and the size below mirror the failing allocation exactly - see
     * the BLE_TX_* definitions. */
    size_t largest = heap_caps_get_largest_free_block(BLE_TX_ALLOC_CAPS);
    size_t need    = (size_t)len + BLE_TX_HDR_BYTES + BLE_TX_ALIGN_SLACK +
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

    int cap = (int)ble_att_mtu(s_conn) - 3;
    if (cap > 0 && len > cap) {
        s_drops++;
        if (s_drops < 5 || s_verbose) {
            ESP_LOGW(TAG, "frame of %d B exceeds the %d B the MTU allows - dropped",
                     len, cap);
        }
        return false;
    }
    int rc = s_rx_no_rsp
                 ? ble_gattc_write_no_rsp_flat(s_conn, s_rx_hnd, data, (uint16_t)len)
                 : ble_gattc_write_flat(s_conn, s_rx_hnd, data, (uint16_t)len, NULL, NULL);
    if (rc != 0) {
        s_drops++;
        if (s_verbose || s_drops < 5) ESP_LOGW(TAG, "write failed rc=%d", rc);
        return false;
    }
    s_tx_frames++;
    return true;
}

static void tel_task(void *arg)
{
    (void)arg;
    char tel[TEL_MAX];
    while (s_run) {
        /* Deferred rescan after a pairing failure. Done here rather than in the
         * GAP callback because that runs on the NimBLE host task, which must
         * not sleep. */
        int64_t due = s_rescan_at_us;
        if (due && esp_timer_get_time() >= due) {
            s_rescan_at_us = 0;
            if (s_state != BLE_LINK_READY) start_scan();
        }

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
    s_tel_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------- rx */

static void feed_rx(const uint8_t *data, int len)
{
    char reply[REPLY_MAX];
    for (int i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_line_pos > 0) {
                s_line[s_line_pos] = '\0';
                s_rx_lines++;
                if (s_verbose) ESP_LOGI(TAG, "RX <%s>", s_line);
                flipper_link_inject(s_line, reply, sizeof(reply));
                if (reply[0]) ble_write(reply, (int)strlen(reply));
                s_line_pos = 0;
            }
        } else if (s_line_pos < LINE_MAX - 1) {
            s_line[s_line_pos++] = c;
        } else {
            s_line_pos = 0;   /* overlong: drop rather than mis-parse */
            s_drops++;
        }
    }
}

/* -------------------------------------------------------------- discovery */

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
    ESP_LOGI(TAG, "link ready: %s [%s] - subscribed, telemetry at %d Hz",
             s_peer_name, s_peer_addr, s_tel_hz);
    /* Announce ourselves the same way the UART transport does. */
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
        /* Pick by GATT properties, not by which UUID is nicknamed "TX".
         * Those names are written from the central's point of view in some
         * references and the peripheral's in others, and getting it backwards
         * is silent: you subscribe to a write-only characteristic that never
         * notifies, and write to a notify-only one that ignores you. The
         * properties bits are unambiguous. */
        bool ours = (ble_uuid_cmp(&chr->uuid.u, &CHR_RX.u) == 0) ||
                    (ble_uuid_cmp(&chr->uuid.u, &CHR_TX.u) == 0);
        if (!ours) return 0;

        if (chr->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) {
            s_tx_hnd = chr->val_handle;
            /* The Flipper's serial service uses INDICATE, not NOTIFY. The CCCD
             * bit differs (0x0002 vs 0x0001) and writing the wrong one is
             * accepted silently - you just never receive anything. */
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
        /* CCCD lives between the TX value handle and the end of the service;
         * a small window is enough and avoids walking the whole database. */
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
        ESP_LOGI(TAG, "serial service at handles %u..%u",
                 svc->start_handle, svc->end_handle);
        int rc = ble_gattc_disc_all_chrs(conn, svc->start_handle,
                                         svc->end_handle, on_chr, NULL);
        if (rc != 0) ESP_LOGE(TAG, "disc chrs rc=%d", rc);
        return 0;
    }

    if (err->status == BLE_HS_EDONE && !s_rx_hnd && !s_tx_hnd) {
        /* Connected to the right device but it is not serving our profile -
         * almost always the head's app is not running, or is running on UART.
         * Reconnecting immediately just repeats this several times a second,
         * which churns NimBLE's mbuf pool hard enough to hit the hard assert in
         * ble_att_tx_with_conn(). Back off and say something useful instead. */
        ESP_LOGE(TAG, "peer has no LakeShark service - is the head's app running "
                      "and set to BLE? backing off %d s", NO_SVC_BACKOFF_S);
        s_rescan_at_us = esp_timer_get_time() + NO_SVC_BACKOFF_S * 1000000LL;
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

/* ---------------------------------------------------------------- scanning */

/* strcasestr is not in newlib's default set here, so roll it. */
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

/* Does this advertisement carry our service UUID?
 *
 * This is the reliable test, and it is better than matching the name in every
 * way that matters: it is true only when the head is actually running the
 * LakeShark app with our profile registered, so we never waste a connect on a
 * Flipper sitting at its desktop, and the operator never has to set
 * `ble name <substr>` to match whatever they called their device. */
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
    /* UUID first - if the head advertises our service, take it regardless of
     * what it is called. The name filter stays as a fallback so a head running
     * an older build (stock serial profile, no UUID of ours in the advert) is
     * still reachable. */
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

    /* Case-insensitive: the Flipper advertises its name with the casing the
     * user set ("Lr1cher1"), which rarely matches how anyone types the filter. */
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
    p.passive       = 0;   /* active: we need the scan response for the name */
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

        /* Log what we hear, not just matches: "scanning, no match" and
         * "scanning, hearing nothing at all" look identical from outside and
         * have completely different causes.
         *
         * But do NOT log every advert. In a busy room that is hundreds of lines
         * a second, all formatted and emitted on the NimBLE host task, which
         * then falls behind draining the SDIO link to the C6 - the log filled
         * with "NimBLE OOM" and the radio eventually died on
         * "assert failed: sdio_push_data_to_queue (pkt_rxbuff)". One sample per
         * second, with the suppressed count, answers the same question. */
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
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            start_scan();
            return 0;
        }
        s_conn    = event->connect.conn_handle;
        s_rx_hnd  = s_tx_hnd = s_tx_cccd = 0;
        s_line_pos = 0;
        s_pair_failed  = false;
        s_pk_wait      = false;
        s_disc_started = false;
        s_state = BLE_LINK_DISCOVERING;

        /* The Flipper demands pairing and drops the link if the central does
         * not oblige ("BleGap: Pairing failed with status: 1", then
         * "Disconnect from client. Reason: 16"). Discovery must wait for
         * encryption, otherwise the connection is torn down mid-walk and every
         * subsequent write is queued against a dead handle - which is exactly
         * how this looked: tx_frames climbing, drops=0, nothing delivered. */
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
        /* The head issues a fresh code per attempt, so anything held from the
         * attempt that just ended is stale. */
        s_pk_early_valid = false;
        s_disc_started = false;

        /* The head regenerates its BT keys on boot - its log says so plainly:
         * "BtKeyStorage: NVRAM sz mismatch" then "BtSrv: Loading new keys". Our
         * stored LTK is then stale, and the head rejects it during encryption
         * setup, i.e. before any ENC_CHANGE reaches us. All we see is the link
         * being cut with HCI 0x05 Authentication Failure, so the bond cleanup
         * on ENC_CHANGE never gets a chance to run and we retry the same dead
         * key several times a second - which on the head looks like it flapping
         * between the app and its pairing screen.
         *
         * Forget the bond here so the next attempt pairs from scratch. */
        if (event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL) ||
            event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)) {
            ESP_LOGW(TAG, "head rejected our stored keys - forgetting the bond, "
                          "expect a fresh passkey on the next attempt");
            ble_store_util_delete_peer(&s_peer_id);
            s_pair_failed = true;   /* take the backoff path below */
        }

        if (!s_run) {
            s_state = BLE_LINK_OFF;
        } else if (s_pair_failed) {
            /* Reconnecting instantly just repeats the same failure a few times
             * a second. Hand the retry to tel_task so the host task is not
             * blocked, and leave the operator a readable log. */
            s_pair_failed  = false;
            s_rescan_at_us = esp_timer_get_time() + 5000000;
            s_state        = BLE_LINK_SCANNING;
            ESP_LOGW(TAG, "waiting 5 s before trying the head again");
        } else if (s_rescan_at_us) {
            /* A backoff is already pending (e.g. the peer had no LakeShark
             * service). Leave it to tel_task rather than scanning right now,
             * which would defeat it. */
            s_state = BLE_LINK_SCANNING;
        } else {
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.attr_handle != s_tx_hnd) return 0;
        struct os_mbuf *om = event->notify_rx.om;
        while (om) {
            feed_rx(om->om_data, om->om_len);
            om = SLIST_NEXT(om, om_next);
        }
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status != 0) {
            /* The teardown below makes the stack report the same failure a
             * second time as BLE_HS_ENOTCONN. Only act on the first. */
            if (s_pair_failed) return 0;
            s_pair_failed = true;
            s_pk_wait     = false;
            ESP_LOGE(TAG, "pairing failed status=%d%s", event->enc_change.status,
                     event->enc_change.status == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_AUTHREQ)
                         ? " (head rejected our authentication requirements)"
                         : event->enc_change.status == BLE_HS_ETIMEOUT
                               ? " (nobody entered the passkey - see \"ble pin\")"
                               : "");

            /* Only forget the bond when the failure actually implicates the
             * stored keys. Deleting on any non-zero status - which is what this
             * did - threw away a good bond whenever the passkey simply went
             * unanswered (ETIMEOUT) or when we tore the link down ourselves
             * ("ble off" surfaces here as ENOTCONN). That is why the head kept
             * displaying a fresh code: every failure, however benign, cost us
             * the bond and forced the whole ceremony again. */
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
        /* Both ends initiate security - we call ble_gap_security_initiate() on
         * connect and the head sends its own SM Security Request - so this
         * event arrives twice, about 450 ms apart. Without a guard we walk the
         * whole GATT database twice and write the CCCD twice per connection. */
        if (s_disc_started) {
            ESP_LOGD(TAG, "encryption re-reported - discovery already running");
            return 0;
        }
        s_disc_started = true;

        /* Raise the ATT MTU before anything is written. The default is 23,
         * which caps a single write at 20 bytes, and a telemetry frame is
         * ~200 - so every frame would be truncated and the head would never
         * see a complete line. Nothing negotiates this for us: NimBLE does not
         * exchange MTU automatically as central, and the head does not ask. */
        int mrc = ble_gattc_exchange_mtu(s_conn, NULL, NULL);
        if (mrc != 0) ESP_LOGW(TAG, "exchange_mtu rc=%d", mrc);

        ESP_LOGI(TAG, "paired, discovering serial service");
        ble_gattc_disc_svc_by_uuid(s_conn, &SVC_SERIAL.u, on_svc, NULL);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Stale bond on our side: drop it and let pairing run again, rather
         * than failing forever because the Flipper regenerated its keys. */
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
            /* The head is displaying a code. We cannot read it, so park the
             * procedure here and let the operator relay it over the console.
             * The Flipper's pairing dialog has no timeout of its own; NimBLE's
             * SM procedure does (BLE_SM_TIMEOUT, 30 s), so this is not an
             * indefinite wait - if it lapses, the reconnect loop tries again. */
            s_pk_conn = event->passkey.conn_handle;
            s_pk_wait = true;

            /* An automated relay reads the code from the head's own log, which
             * the head emits ~100 ms BEFORE this event reaches us. Such a code
             * used to arrive while s_pk_wait was still false and be thrown
             * away, and pairing then died 30 s later with status 13 "nobody
             * entered the passkey" - reproducible every time with a script,
             * while a human typing slowly always landed inside the window.
             * Accept an early code and apply it here. */
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
            /* Not reachable with the stock serial profile, but a head built
             * with GapPairingPinCodeVerifyYesNo would land here and there is
             * nothing to ask a headless box - the operator confirms on the
             * head's own screen. */
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

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU now %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------- init */

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
    nimble_port_run();              /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_link_start(void)
{
    if (s_run) return ESP_ERR_INVALID_STATE;

    /* Everything below this point is one-time setup. ble_link_stop() only stops
     * scanning and drops the connection - it deliberately leaves the host stack
     * and the co-processor's controller up, because tearing NimBLE down and
     * bringing it back is not something the esp_hosted transport survives
     * cleanly. Re-running the init would fail on
     * esp_hosted_bt_controller_init() -> ESP_ERR_INVALID_STATE (already
     * initialised), which used to make "ble off" a one-way door: the radio
     * could not be turned back on without rebooting the board. */
    if (s_stack_up) {
        s_run   = true;
        s_state = BLE_LINK_SCANNING;
        s_rescan_at_us = 0;
        /* tel_task returns as soon as s_run goes false, so a restart has to
         * recreate it. Without this the link comes up and answers commands
         * perfectly - the reply path is driven by RX - but never sends a single
         * telemetry frame, which looks like a dead UI on a healthy link. */
        if (!s_tel_task) {
            xTaskCreate(tel_task, "ble_tel", 6144, NULL, 4, &s_tel_task);
        }
        start_scan();
        return ESP_OK;
    }

    /* The BT controller lives on the C6 and does NOT start with the transport.
     * The slave only runs init_bluetooth()/enable_bluetooth() when the host
     * asks over RPC (slave_control.c, RPC_FEATURE__Feature_Bluetooth). Without
     * this the co-processor still advertises "HCI over SDIO / BLE only" in its
     * capabilities - it just never brings the controller up - and every HCI
     * command from NimBLE times out as BLE_HS_ETIMEOUT_HCI on HCI_Reset. */
    esp_err_t rc = esp_hosted_bt_controller_init();
    ESP_LOGI(TAG, "co-processor bt_controller_init: %s", esp_err_to_name(rc));
    if (rc != ESP_OK) return rc;

    rc = esp_hosted_bt_controller_enable();
    ESP_LOGI(TAG, "co-processor bt_controller_enable: %s", esp_err_to_name(rc));
    if (rc != ESP_OK) return rc;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Install the key store. Without it the security manager refuses to pair,
     * and the way it refuses is thoroughly misleading:
     *
     *   ble_gap_security_initiate -> ble_sm_pair_initiate
     *     -> ble_sm_chk_store_overflow      (counts existing bonds first)
     *       -> ble_store_util_count -> ble_store_iterate -> ble_store_read
     *         -> ble_hs_cfg.store_read_cb == NULL -> BLE_HS_ENOTSUP
     *
     * ble_sm_pair_initiate() returns that rc verbatim before allocating a proc,
     * so ble_gap_security_initiate() reports ENOTSUP (8) even though the SM is
     * fully compiled in - which is what the NIMBLE_BLE_SM=1 log below proves.
     * No HCI command is emitted on this path, so the controller is not
     * involved and cannot be at fault.
     *
     * The peer-initiated path fails identically: ble_sm_sec_req_rx() also ends
     * in ble_sm_pair_initiate(), so when the Flipper sends its SM Security
     * Request we never answer, it times out and terminates the link with HCI
     * 0x05 Authentication Failure (NimBLE reason=517).
     *
     * ESP-IDF does not wire this up for you; every NimBLE example calls it. */
    ble_store_config_init();

    /* ble_gap_security_initiate() has exactly one ENOTSUP path of its own: the
     * #else of "#if NIMBLE_BLE_SM". Print what the compiler actually resolved
     * so a Kconfig that looks correct in sdkconfig.h cannot lie about it. */
    ESP_LOGW(TAG, "NIMBLE_BLE_SM=%d BLE_SM_LEGACY=%d BLE_SM_SC=%d",
             (int)NIMBLE_BLE_SM, (int)MYNEWT_VAL(BLE_SM_LEGACY),
             (int)MYNEWT_VAL(BLE_SM_SC));

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* Passkey entry, with the console as our keyboard.
     *
     * This is forced by the head, not a preference. serial_profile.c configures
     * the Flipper's serial profile as
     *
     *     .pairing_method = GapPairingPinCodeShow
     *
     * and gap.c turns that into IO_CAP_DISPLAY_ONLY with
     * MITM_PROTECTION_REQUIRED. Against a DisplayOnly/MITM peer the only
     * association model that satisfies MITM is Passkey Entry with the peer
     * displaying and the initiator typing - so the central must claim a
     * keyboard. NO_INPUT_OUTPUT (or any display-only cap) negotiates down to
     * Just Works, which cannot meet MITM, and the Flipper rejects the pairing
     * request outright with SM error 0x03 AUTHREQ - surfacing here as
     * BLE_GAP_EVENT_ENC_CHANGE status=1283 (0x503 = SM_PEER_BASE + 0x03).
     *
     * The head generates a random 6-digit code, shows it on its screen, and we
     * feed it back with "ble pin <code>". Bonding is on and
     * CONFIG_BT_NIMBLE_NVS_PERSIST=y, so this is a one-time ceremony: every
     * later reconnect restores the LTK from NVS without prompting.
     *
     * ---------------------------------------------------------------------
     * That was all forced by the *serial* profile's GAP config, which is
     * DisplayOnly with MITM required. The head now registers a profile of its
     * own (ls_ble_profile.c) that asks for GapPairingNone, so Just Works is
     * acceptable to both ends and no passkey is involved at all: the link comes
     * up unattended after either side reboots.
     *
     * NO_INPUT_OUTPUT + mitm=0 is what negotiates Just Works. Bonding and
     * secure connections stay on, so the link is still encrypted and the LTK is
     * still persisted; what is given up is MITM protection during pairing,
     * which for two boards sitting on the same desk buys nothing.
     *
     * If a head is ever built with the stock serial profile again, put these
     * back to KEYBOARD_ONLY / mitm=1 and use `ble pin`. The passkey plumbing is
     * still present and still works. */
    ble_hs_cfg.sm_io_cap        = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding       = 1;
    ble_hs_cfg.sm_mitm          = 0;
    ble_hs_cfg.sm_sc            = 1;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb  = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gap_device_name_set("LakeShark");

    s_stack_up = true;
    s_run      = true;
    s_state    = BLE_LINK_SYNCING;
    nimble_port_freertos_init(host_task);

    if (!s_tel_task) {
        xTaskCreate(tel_task, "ble_tel", 6144, NULL, 4, &s_tel_task);
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
        /* Too early rather than wrong: the head announces the code just before
         * our SM raises the request. Hold it and let BLE_SM_IOACT_INPUT use it,
         * instead of discarding it and stalling until the 30 s SM timeout. */
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
