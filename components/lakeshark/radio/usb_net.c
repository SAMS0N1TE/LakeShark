/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. USB Ethernet adapters on the USB host port: CDC-ECM
   (RTL8152/8153 in their ECM configuration, most "driverless" adapters, Linux
   gadgets) and RNDIS (Android USB tethering). A second device type on the
   class driver's USB client, beside the RTL-SDR and HackRF adapters; one
   adapter at a time.

   Why not esp-iot-solution's iot_usbh_ecm/iot_usbh_rndis (Apache-2.0): they
   sit on iot_usbh_cdc, which registers its own USB client, opens every device
   that enumerates (aborting on a descriptor error) and reads bulk IN through
   a byte ring buffer, so two frames that arrive together become one. Here a
   bulk IN transfer is exactly one ECM frame (it ends on a short packet) and
   the SDR dispatch stays in one place.

   Contexts: transfer callbacks and DEV_GONE run in the class driver task (the
   USB client); setup and teardown block on control transfers, so each runs in
   its own short task with a PSRAM stack. */
#include "sdkconfig.h"

#if CONFIG_LS_USB_ETH

#include "usb_net.h"
#include "usb_net_desc.h"
#include "usb/usb_helpers.h"
#include "usb_host.h"
#include "rtl_adapter_private.h"
#include "ls_task_reap.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "usb_net";

#define N_RX        4
#define N_TX        4
#define CTRL_DATA   512u      /* control data stage; RNDIS replies we ask for are < 100 */
#define LANG_EN_US  0x0409
#define ECM_SET_PACKET_FILTER 0x43
#define ECM_FILTER_DEFAULT    0x0E  /* directed, broadcast, all multicast */
#define LINK_ASSUME_MS        3000  /* an ECM adapter that never notifies */

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    ls_usbnet_desc_t d;
    uint16_t vid, pid;
    uint8_t ep0_mps;
    uint8_t mac[6];
    usb_transfer_t *rx[N_RX], *tx[N_TX], *notify, *resp;
    uint32_t tx_free;          /* bit i: tx[i] idle */
    int inflight;              /* transfers submitted and not yet returned */
    bool gone, resp_busy, ctrl_claimed, data_claimed;
    bool link, link_seen;
    uint32_t bps, rx_errors, tx_drops;
    unsigned rx_error_run;
} usbnet_t;

typedef struct {
    uint8_t addr;
    usb_host_client_handle_t client;
    bool search;
} setup_arg_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static usbnet_t *s_net;                  /* published adapter */
static usb_device_handle_t s_setup_dev;  /* device being set up */
static bool s_setup_active, s_setup_gone;
static const ls_usb_net_sink_t *s_sink;

void ls_usb_net_set_sink(const ls_usb_net_sink_t *sink)
{
    __atomic_store_n(&s_sink, sink, __ATOMIC_RELEASE);
}
static const ls_usb_net_sink_t *sink(void) { return __atomic_load_n(&s_sink, __ATOMIC_ACQUIRE); }

static void inflight_add(usbnet_t *n, int v) { __atomic_add_fetch(&n->inflight, v, __ATOMIC_ACQ_REL); }
static bool is_gone(usbnet_t *n) { return __atomic_load_n(&n->gone, __ATOMIC_ACQUIRE); }

/* ---- synchronous control transfers (setup/teardown tasks only) ---- */

static void ctrl_done(usb_transfer_t *t) { xSemaphoreGive((SemaphoreHandle_t)t->context); }

/* Returns the data stage length in *got. A transfer the device never answers
   is left allocated (its callback may still come): *t is set NULL. */
static esp_err_t ctrl(usbnet_t *n, usb_transfer_t **tp, SemaphoreHandle_t sem, uint8_t bm,
                      uint8_t req, uint16_t value, uint16_t index, const void *out,
                      uint16_t len, size_t *got)
{
    usb_transfer_t *t = *tp;
    if (!t || len > CTRL_DATA) return ESP_ERR_INVALID_STATE;
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = bm; s->bRequest = req; s->wValue = value; s->wIndex = index; s->wLength = len;
    const bool in = bm & 0x80;
    if (!in && len) memcpy(t->data_buffer + sizeof(*s), out, len);
    t->num_bytes = sizeof(*s) + (in ? usb_round_up_to_mps(len, n->ep0_mps) : len);
    t->device_handle = n->dev;
    t->bEndpointAddress = 0;
    t->callback = ctrl_done;
    t->context = sem;
    t->timeout_ms = 1000;
    xSemaphoreTake(sem, 0);
    esp_err_t e = usb_host_transfer_submit_control(n->client, t);
    if (e != ESP_OK) return e;
    if (xSemaphoreTake(sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "control 0x%02x/0x%02x never completed", bm, req);
        *tp = NULL;
        return ESP_ERR_TIMEOUT;
    }
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) return ESP_FAIL;
    const int data = t->actual_num_bytes - (int)sizeof(*s);
    if (got) *got = data > 0 ? (size_t)data : 0;
    return ESP_OK;
}

/* ---- RNDIS control channel ---- */

static esp_err_t rndis_call(usbnet_t *n, usb_transfer_t **t, SemaphoreHandle_t sem,
                            const uint8_t *msg, size_t msg_len, uint32_t reply_type,
                            uint32_t req_id, uint8_t *reply, size_t *reply_len)
{
    const uint16_t intf = n->d.ctrl_intf;
    esp_err_t e = ctrl(n, t, sem, 0x21, 0x00, 0, intf, msg, (uint16_t)msg_len, NULL);
    if (e != ESP_OK) return e;
    /* Poll for the reply rather than wait for RESPONSE_AVAILABLE: the
       notification transfer is not running yet. Indications seen meanwhile
       are dropped; the media status is queried explicitly. */
    for (int i = 0; i < 25; ++i) {
        size_t got = 0;
        e = ctrl(n, t, sem, 0xA1, 0x01, 0, intf, NULL, CTRL_DATA, &got);
        if (e == ESP_ERR_TIMEOUT) return e;
        if (e == ESP_OK && got >= 16 &&
            ls_rndis_parse_reply((*t)->data_buffer + sizeof(usb_setup_packet_t), got, reply_type,
                                 req_id, NULL, NULL, NULL)) {
            memcpy(reply, (*t)->data_buffer + sizeof(usb_setup_packet_t), got);
            *reply_len = got;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t rndis_query_u32(usbnet_t *n, usb_transfer_t **t, SemaphoreHandle_t sem,
                                 uint32_t id, uint32_t oid, uint8_t *val, size_t want)
{
    uint8_t msg[32], reply[CTRL_DATA];
    size_t len = ls_rndis_build_query(msg, sizeof(msg), id, oid), reply_len = 0;
    esp_err_t e = rndis_call(n, t, sem, msg, len, RNDIS_MSG_QUERY_C, id, reply, &reply_len);
    if (e != ESP_OK) return e;
    const uint8_t *info = NULL; size_t info_len = 0;
    uint32_t status = 1;
    if (!ls_rndis_parse_reply(reply, reply_len, RNDIS_MSG_QUERY_C, id, &status, &info, &info_len) ||
        status || info_len < want)
        return ESP_FAIL;
    memcpy(val, info, want);
    return ESP_OK;
}

static esp_err_t rndis_init(usbnet_t *n, usb_transfer_t **t, SemaphoreHandle_t sem)
{
    uint8_t msg[32], reply[CTRL_DATA];
    size_t reply_len = 0;
    size_t len = ls_rndis_build_init(msg, sizeof(msg), 1, LS_USBNET_RX_BUFFER);
    esp_err_t e = rndis_call(n, t, sem, msg, len, RNDIS_MSG_INIT_C, 1, reply, &reply_len);
    if (e != ESP_OK) return e;
    uint32_t status = 1;
    if (!ls_rndis_parse_reply(reply, reply_len, RNDIS_MSG_INIT_C, 1, &status, NULL, NULL) || status)
        return ESP_FAIL;
    e = rndis_query_u32(n, t, sem, 2, RNDIS_OID_PERMANENT_ADDR, n->mac, 6);
    if (e != ESP_OK) return e;
    len = ls_rndis_build_set_u32(msg, sizeof(msg), 3, RNDIS_OID_PACKET_FILTER, RNDIS_FILTER_DEFAULT);
    e = rndis_call(n, t, sem, msg, len, RNDIS_MSG_SET_C, 3, reply, &reply_len);
    status = 1;
    if (e != ESP_OK) return e;
    if (!ls_rndis_parse_reply(reply, reply_len, RNDIS_MSG_SET_C, 3, &status, NULL, NULL) || status)
        return ESP_FAIL;
    uint8_t v[4];
    if (rndis_query_u32(n, t, sem, 4, RNDIS_OID_MEDIA_STATUS, v, 4) == ESP_OK) {
        n->link = (v[0] | v[1] | v[2] | v[3]) == 0; /* 0 = connected */
        n->link_seen = true;
    }
    if (rndis_query_u32(n, t, sem, 5, RNDIS_OID_LINK_SPEED, v, 4) == ESP_OK)
        n->bps = (v[0] | (v[1] << 8) | (v[2] << 16) | ((uint32_t)v[3] << 24)) * 100u;
    return ESP_OK;
}

/* ---- data path (USB client task) ---- */

static void deliver(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    const ls_usb_net_sink_t *s = sink();
    if (s && s->rx) s->rx(frame, len);
}

static void link_changed(usbnet_t *n)
{
    const ls_usb_net_sink_t *s = sink();
    if (s && s->link) s->link(n->link, n->bps);
}

static void resubmit(usbnet_t *n, usb_transfer_t *t)
{
    if (is_gone(n) || usb_host_transfer_submit(t) != ESP_OK) inflight_add(n, -1);
}

/* Returns false when the transfer must not be resubmitted. */
static bool transfer_ok(usbnet_t *n, usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) { n->rx_error_run = 0; return true; }
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED ||
        t->status == USB_TRANSFER_STATUS_STALL || ++n->rx_error_run > 32) {
        if (t->status == USB_TRANSFER_STATUS_STALL) ESP_LOGW(TAG, "endpoint 0x%02x stalled", t->bEndpointAddress);
        inflight_add(n, -1);
        return false;
    }
    __atomic_add_fetch(&n->rx_errors, 1, __ATOMIC_RELAXED);
    return true;
}

static void rx_done(usb_transfer_t *t)
{
    usbnet_t *n = t->context;
    if (!transfer_ok(n, t)) return;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0) {
        if (n->d.kind == LS_USBNET_RNDIS) {
            ls_rndis_unwrap(t->data_buffer, (size_t)t->actual_num_bytes, deliver, NULL);
        } else if (ls_usbnet_frame_ok((size_t)t->actual_num_bytes)) {
            deliver(t->data_buffer, (size_t)t->actual_num_bytes, NULL);
        } else {
            __atomic_add_fetch(&n->rx_errors, 1, __ATOMIC_RELAXED);
        }
    }
    resubmit(n, t);
}

static void resp_done(usb_transfer_t *t)
{
    usbnet_t *n = t->context;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        const int got = t->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
        const int up = got > 0 ? ls_rndis_parse_indicate(t->data_buffer + sizeof(usb_setup_packet_t),
                                                         (size_t)got) : -1;
        if (up >= 0) { n->link = up; n->link_seen = true; link_changed(n); }
    }
    __atomic_store_n(&n->resp_busy, false, __ATOMIC_RELEASE);
    inflight_add(n, -1);
}

static void resp_kick(usbnet_t *n)
{
    if (!n->resp || is_gone(n) || __atomic_exchange_n(&n->resp_busy, true, __ATOMIC_ACQ_REL)) return;
    usb_transfer_t *t = n->resp;
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = 0xA1; s->bRequest = 0x01; s->wValue = 0;
    s->wIndex = n->d.ctrl_intf; s->wLength = CTRL_DATA;
    t->num_bytes = sizeof(*s) + usb_round_up_to_mps(CTRL_DATA, n->ep0_mps);
    t->device_handle = n->dev; t->bEndpointAddress = 0;
    t->callback = resp_done; t->context = n;
    inflight_add(n, 1);
    if (usb_host_transfer_submit_control(n->client, t) != ESP_OK) {
        __atomic_store_n(&n->resp_busy, false, __ATOMIC_RELEASE);
        inflight_add(n, -1);
    }
}

static void notify_done(usb_transfer_t *t)
{
    usbnet_t *n = t->context;
    if (!transfer_ok(n, t)) return;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        bool link = n->link; uint32_t bps = n->bps;
        const unsigned set = ls_usbnet_parse_notification(t->data_buffer,
                                                          (size_t)t->actual_num_bytes, &link, &bps);
        if (set & LS_USBNET_NOTE_SPEED) n->bps = bps;
        if (set & LS_USBNET_NOTE_LINK) {
            n->link = link;
            __atomic_store_n(&n->link_seen, true, __ATOMIC_RELEASE);
        }
        if (set & (LS_USBNET_NOTE_LINK | LS_USBNET_NOTE_SPEED)) link_changed(n);
        if ((set & LS_USBNET_NOTE_RESPONSE) && n->d.kind == LS_USBNET_RNDIS) resp_kick(n);
    }
    resubmit(n, t);
}

static void tx_done(usb_transfer_t *t)
{
    usbnet_t *n = t->context;
    for (unsigned i = 0; i < N_TX; ++i)
        if (n->tx[i] == t) __atomic_or_fetch(&n->tx_free, 1u << i, __ATOMIC_ACQ_REL);
    inflight_add(n, -1);
}

esp_err_t ls_usb_net_tx(const void *frame, size_t len)
{
    if (!frame || !ls_usbnet_frame_ok(len)) return ESP_ERR_INVALID_SIZE;
    portENTER_CRITICAL(&s_lock);
    usbnet_t *n = s_net;
    if (n) inflight_add(n, 1); /* holds n against teardown */
    portEXIT_CRITICAL(&s_lock);
    if (!n) return ESP_ERR_INVALID_STATE;
    if (is_gone(n)) { inflight_add(n, -1); return ESP_ERR_INVALID_STATE; }
    uint32_t free_bits = __atomic_load_n(&n->tx_free, __ATOMIC_ACQUIRE);
    int slot = -1;
    while (free_bits) {
        const int i = __builtin_ctz(free_bits);
        if (__atomic_compare_exchange_n(&n->tx_free, &free_bits, free_bits & ~(1u << i), false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { slot = i; break; }
    }
    if (slot < 0) {
        __atomic_add_fetch(&n->tx_drops, 1, __ATOMIC_RELAXED);
        inflight_add(n, -1);
        return ESP_ERR_NO_MEM;
    }
    usb_transfer_t *t = n->tx[slot];
    size_t total = len;
    if (n->d.kind == LS_USBNET_RNDIS) {
        ls_rndis_packet_header(t->data_buffer, len);
        memcpy(t->data_buffer + LS_RNDIS_HEADER, frame, len);
        total += LS_RNDIS_HEADER;
    } else {
        memcpy(t->data_buffer, frame, len);
    }
    t->num_bytes = total;
    t->flags = ls_usbnet_tx_needs_zlp(total, n->d.out_mps) ? USB_TRANSFER_FLAG_ZERO_PACK : 0;
    if (usb_host_transfer_submit(t) != ESP_OK) {
        __atomic_or_fetch(&n->tx_free, 1u << slot, __ATOMIC_ACQ_REL);
        __atomic_add_fetch(&n->tx_drops, 1, __ATOMIC_RELAXED);
        inflight_add(n, -1);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- lifetime ---- */

static esp_err_t alloc_transfer(usbnet_t *n, size_t size, uint8_t ep, usb_transfer_cb_t cb,
                                usb_transfer_t **out)
{
    esp_err_t e = usb_host_transfer_alloc(size, 0, out);
    if (e != ESP_OK) return e;
    (*out)->device_handle = n->dev;
    (*out)->bEndpointAddress = ep;
    (*out)->callback = cb;
    (*out)->context = n;
    (*out)->num_bytes = size;
    return ESP_OK;
}

/* Stops every transfer and gives the device back. Blocking; never from the
   USB client task, whose callbacks it waits for. */
static void teardown(usbnet_t *n)
{
    __atomic_store_n(&n->gone, true, __ATOMIC_RELEASE);
    const uint8_t eps[3] = {n->d.in_ep, n->d.out_ep, n->d.notify_ep};
    for (int waited = 0; __atomic_load_n(&n->inflight, __ATOMIC_ACQUIRE) > 0 && waited < 2000;
         waited += 50) {
        for (int i = 0; i < 3; ++i) {
            if (!eps[i] || (i == 2 && !n->notify)) continue;
            usb_host_endpoint_halt(n->dev, eps[i]);
            usb_host_endpoint_flush(n->dev, eps[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    const bool idle = __atomic_load_n(&n->inflight, __ATOMIC_ACQUIRE) <= 0;
    if (!idle) ESP_LOGE(TAG, "transfers did not drain; leaving them allocated");
    for (int i = 0; i < 3; ++i)
        if (eps[i]) usb_host_endpoint_clear(n->dev, eps[i]);
    if (n->data_claimed) usb_host_interface_release(n->client, n->dev, n->d.data_intf);
    if (n->ctrl_claimed && n->d.ctrl_intf != n->d.data_intf)
        usb_host_interface_release(n->client, n->dev, n->d.ctrl_intf);
    if (idle) {
        for (int i = 0; i < N_RX; ++i) if (n->rx[i]) usb_host_transfer_free(n->rx[i]);
        for (int i = 0; i < N_TX; ++i) if (n->tx[i]) usb_host_transfer_free(n->tx[i]);
        if (n->notify) usb_host_transfer_free(n->notify);
        if (n->resp) usb_host_transfer_free(n->resp);
    }
    if (n->dev) usb_host_device_close(n->client, n->dev);
    if (idle) free(n);
}

static void cleanup_task(void *arg)
{
    usbnet_t *n = arg;
    teardown(n);
    ESP_LOGI(TAG, "adapter released");
    ls_task_retire_self();
}

bool ls_usb_net_note_removed(usb_device_handle_t device)
{
    portENTER_CRITICAL(&s_lock);
    usbnet_t *n = s_net && s_net->dev == device ? s_net : NULL;
    if (n) s_net = NULL;
    const bool setup = !n && s_setup_active && s_setup_dev == device;
    if (setup) s_setup_gone = true;
    portEXIT_CRITICAL(&s_lock);
    if (!n) return setup;
    __atomic_store_n(&n->gone, true, __ATOMIC_RELEASE);
    const ls_usb_net_sink_t *s = sink();
    if (s && s->detached) s->detached();
    ESP_LOGW(TAG, "USB Ethernet adapter removed");
    if (xTaskCreatePinnedToCoreWithCaps(cleanup_task, "usbnet_gone", 3072, n, 4, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE(TAG, "no memory for the cleanup task; adapter objects retained");
    return true;
}

bool ls_usb_net_enum_filter(const usb_device_desc_t *dev_desc, uint8_t *config_value)
{
    const uint8_t c = ls_usbnet_pick_config(dev_desc->idVendor, dev_desc->idProduct,
                                            dev_desc->bNumConfigurations);
    if (c != *config_value) {
        ESP_LOGI(TAG, "%04x:%04x: configuration %u (network)", dev_desc->idVendor,
                       dev_desc->idProduct, c);
        *config_value = c;
    }
    return true;
}

/* The active configuration has no network function. If another one has
   (RTL8152/8153 keep ECM in configuration 2), remember it and re-enumerate
   through a root-port cycle; otherwise the device is not ours. */
static bool search_configs(usbnet_t *n, const usb_device_desc_t *dd, uint8_t active)
{
    uint8_t target = 0;
    for (uint8_t cv = 1; cv <= dd->bNumConfigurations && !target; ++cv) {
        if (cv == active) continue;
        const usb_config_desc_t *cd = NULL;
        if (usb_host_get_config_desc(n->client, n->dev, cv, &cd) != ESP_OK || !cd) continue;
        ls_usbnet_desc_t d;
        if (ls_usbnet_parse_config((const uint8_t *)cd, cd->wTotalLength, &d) && d.config_value == cv)
            target = cv;
        usb_host_free_config_desc(cd);
    }
    if (!target) return false;
    ESP_LOGW(TAG, "%04x:%04x has a network function in configuration %u; re-enumerating with it",
             n->vid, n->pid, target);
    ls_usbnet_learn_config(n->vid, n->pid, target);
    return true;
}

static esp_err_t ecm_init(usbnet_t *n, usb_transfer_t **t, SemaphoreHandle_t sem)
{
    /* SET_INTERFACE 0 then the data alt resets the function's filters, as
       Linux cdc_ether does; claiming only sets up our pipes. */
    if (n->d.data_alt) {
        (void)ctrl(n, t, sem, 0x01, 0x0B, 0, n->d.data_intf, NULL, 0, NULL);
        esp_err_t e = ctrl(n, t, sem, 0x01, 0x0B, n->d.data_alt, n->d.data_intf, NULL, 0, NULL);
        if (e != ESP_OK) return e;
    }
    size_t got = 0;
    bool mac = false;
    if (n->d.mac_string &&
        ctrl(n, t, sem, 0x80, 0x06, (uint16_t)(0x0300 | n->d.mac_string), LANG_EN_US, NULL, 64,
             &got) == ESP_OK)
        mac = ls_usbnet_parse_mac_string((*t)->data_buffer + sizeof(usb_setup_packet_t), got, n->mac);
    if (!mac) {
        uint8_t base[6];
        esp_read_mac(base, ESP_MAC_ETH);
        esp_derive_local_mac(n->mac, base);
        ESP_LOGW(TAG, "no iMACAddress; using a local address");
    }
    if (!*t) return ESP_ERR_TIMEOUT;
    if (ctrl(n, t, sem, 0x21, ECM_SET_PACKET_FILTER, ECM_FILTER_DEFAULT, n->d.ctrl_intf, NULL, 0,
             NULL) != ESP_OK)
        ESP_LOGW(TAG, "SET_ETHERNET_PACKET_FILTER refused; continuing");
    return *t ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void usbnet_setup_task(void *arg)
{
    setup_arg_t a = *(setup_arg_t *)arg;
    free(arg);
    usbnet_t *n = calloc(1, sizeof(*n));
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    usb_transfer_t *ct = NULL;
    bool ct_made = false, to_sdr = false, cycle = false, published = false;
    esp_err_t e = ESP_ERR_NO_MEM;
    if (!n || !sem) goto out;
    n->client = a.client;
    if (usb_host_device_open(a.client, a.addr, &n->dev) != ESP_OK) { n->dev = NULL; goto out; }
    portENTER_CRITICAL(&s_lock);
    s_setup_dev = n->dev;
    portEXIT_CRITICAL(&s_lock);
    const usb_device_desc_t *dd = NULL;
    const usb_config_desc_t *cd = NULL;
    if (usb_host_get_device_descriptor(n->dev, &dd) != ESP_OK || !dd ||
        usb_host_get_active_config_descriptor(n->dev, &cd) != ESP_OK || !cd)
        goto out;
    n->vid = dd->idVendor; n->pid = dd->idProduct; n->ep0_mps = dd->bMaxPacketSize0;
    if (!ls_usbnet_parse_config((const uint8_t *)cd, cd->wTotalLength, &n->d)) {
        if (a.search && search_configs(n, dd, cd->bConfigurationValue)) cycle = true;
        else to_sdr = true;
        goto out;
    }
    const char *kind = n->d.kind == LS_USBNET_RNDIS ? "RNDIS" : "ECM";
    ESP_LOGI(TAG, "%04x:%04x %s: ctrl %u data %u/%u in 0x%02x out 0x%02x notify 0x%02x", n->vid,
             n->pid, kind, n->d.ctrl_intf, n->d.data_intf, n->d.data_alt, n->d.in_ep, n->d.out_ep,
             n->d.notify_ep);

    if ((e = usb_host_interface_claim(n->client, n->dev, n->d.ctrl_intf, 0)) != ESP_OK) goto out;
    n->ctrl_claimed = true;
    if (n->d.data_intf != n->d.ctrl_intf) {
        if ((e = usb_host_interface_claim(n->client, n->dev, n->d.data_intf, n->d.data_alt)) != ESP_OK)
            goto out;
        n->data_claimed = true;
    }
    if ((e = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + CTRL_DATA, 0, &ct)) != ESP_OK)
        goto out;
    ct_made = true;
    e = n->d.kind == LS_USBNET_RNDIS ? rndis_init(n, &ct, sem) : ecm_init(n, &ct, sem);
    if (e != ESP_OK) goto out;

    e = ESP_ERR_NO_MEM;
    for (int i = 0; i < N_RX; ++i)
        if (alloc_transfer(n, LS_USBNET_RX_BUFFER, n->d.in_ep, rx_done, &n->rx[i]) != ESP_OK) goto out;
    for (int i = 0; i < N_TX; ++i) {
        if (alloc_transfer(n, LS_USBNET_RX_BUFFER, n->d.out_ep, tx_done, &n->tx[i]) != ESP_OK) goto out;
        n->tx_free |= 1u << i;
    }
    if (n->d.notify_ep && n->d.notify_mps &&
        alloc_transfer(n, usb_round_up_to_mps(16, n->d.notify_mps), n->d.notify_ep, notify_done,
                       &n->notify) != ESP_OK)
        goto out;
    if (n->d.kind == LS_USBNET_RNDIS &&
        usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + usb_round_up_to_mps(CTRL_DATA, n->ep0_mps),
                                0, &n->resp) != ESP_OK)
        goto out;
    e = ESP_OK;
    for (int i = 0; i < N_RX && e == ESP_OK; ++i) {
        inflight_add(n, 1);
        if ((e = usb_host_transfer_submit(n->rx[i])) != ESP_OK) inflight_add(n, -1);
    }
    if (e == ESP_OK && n->notify) {
        inflight_add(n, 1);
        if ((e = usb_host_transfer_submit(n->notify)) != ESP_OK) inflight_add(n, -1);
    }
    if (e != ESP_OK) goto out;

    portENTER_CRITICAL(&s_lock);
    published = !s_setup_gone && !s_net;
    if (published) {
        s_net = n;
        inflight_add(n, 1); /* this task's hold on n until the link wait ends */
    }
    s_setup_dev = NULL;
    portEXIT_CRITICAL(&s_lock);
    if (!published) { e = ESP_ERR_INVALID_STATE; goto out; }
    ESP_LOGI(TAG, "%s adapter %02x:%02x:%02x:%02x:%02x:%02x", kind, n->mac[0], n->mac[1], n->mac[2],
             n->mac[3], n->mac[4], n->mac[5]);
    const ls_usb_net_sink_t *s = sink();
    if (s && s->attached) s->attached(n->mac);
    if (n->link_seen) link_changed(n);
    /* ECM adapters normally say NETWORK_CONNECTION within a second. */
    for (int waited = 0; !__atomic_load_n(&n->link_seen, __ATOMIC_ACQUIRE) && !is_gone(n) &&
                         waited < LINK_ASSUME_MS;
         waited += 100)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (!is_gone(n) && !__atomic_load_n(&n->link_seen, __ATOMIC_ACQUIRE)) {
        ESP_LOGW(TAG, "no link notification; assuming link up");
        n->link = true;
        link_changed(n);
    }
    inflight_add(n, -1); /* teardown may free n from here on */

out:
    if (!published && n && e != ESP_OK && !to_sdr && !cycle)
        ESP_LOGW(TAG, "adapter setup failed: %s", esp_err_to_name(e));
    if (ct) usb_host_transfer_free(ct);
    /* A control transfer that never completed may still give the semaphore. */
    if (sem && !(ct_made && !ct)) vSemaphoreDelete(sem);
    portENTER_CRITICAL(&s_lock);
    s_setup_dev = NULL;
    s_setup_active = false;
    s_setup_gone = false;
    portEXIT_CRITICAL(&s_lock);
    if (!published && n) {
        if (n->dev) teardown(n);
        else free(n);
    }
    if (to_sdr) rtl_adapter_probe_async(a.addr, a.client);
    if (cycle) {
        const usb_port_cycle_result_t r = usb_host_root_port_cycle();
        ESP_LOGI(TAG, "re-enumeration: %s", usb_port_cycle_result_name(r));
    }
    ls_task_retire_self();
}

void ls_usb_net_probe_async(uint8_t dev_addr, usb_host_client_handle_t client, bool search)
{
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_setup_active || s_net;
    if (!busy) s_setup_active = true;
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        ESP_LOGW(TAG, "one USB Ethernet adapter at a time; ignoring addr %u", dev_addr);
        return;
    }
    setup_arg_t *a = malloc(sizeof(*a));
    if (a) *a = (setup_arg_t){.addr = dev_addr, .client = client, .search = search};
    if (!a || xTaskCreatePinnedToCoreWithCaps(usbnet_setup_task, "usbnet_setup", 4096, a, 4, NULL, 1,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "setup task create failed");
        free(a);
        portENTER_CRITICAL(&s_lock);
        s_setup_active = false;
        portEXIT_CRITICAL(&s_lock);
    }
}

void ls_usb_net_status(ls_usb_net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_lock);
    usbnet_t *n = s_net;
    if (n) {
        out->present = true;
        out->link = n->link;
        memcpy(out->mac, n->mac, 6);
        out->bps = n->bps;
        out->kind = n->d.kind == LS_USBNET_RNDIS ? "RNDIS" : "ECM";
        out->vid = n->vid; out->pid = n->pid;
        out->rx_errors = n->rx_errors; out->tx_drops = n->tx_drops;
    }
    portEXIT_CRITICAL(&s_lock);
}

#endif /* CONFIG_LS_USB_ETH */
