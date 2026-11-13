/* Wi-Fi monitor mode through the coprocessor, for capture apps.

   esp_hosted 2.12.9 leaves esp_wifi_remote_set_promiscuous* unimplemented
   (weak stubs answer ESP_ERR_NOT_SUPPORTED). A coprocessor built with
   LakeShark's forwarder (c5-slave ls_promisc.c) takes CTRL messages over the
   peer-data channel and sends each frame back as a DATA message. These are
   the real esp_wifi_remote_* calls on top of that. A coprocessor without the
   forwarder never answers, so enabling still reports ESP_ERR_NOT_SUPPORTED
   and capture apps say the coprocessor firmware lacks it. */
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi_remote.h"
#include "esp_hosted_misc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LS_PROMISC_CTRL 0x4C530101u   /* host -> coprocessor */
#define LS_PROMISC_ACK  0x4C530102u   /* coprocessor -> host */
#define LS_PROMISC_DATA 0x4C530103u   /* coprocessor -> host */
#define LS_PROMISC_VER  1
#define ACK_WAIT_MS     800
#define SNAP            512           /* bytes of each frame forwarded */
#define MAX_PER_S       250           /* frames a second the coprocessor forwards */

enum { OP_PING = 0, OP_SET = 1, OP_CHANNEL = 2 };

typedef struct __attribute__((packed)) {
    uint8_t ver, op, enable, channel;
    uint32_t filter_mask;
    uint16_t snap, max_per_s;
} ctrl_t;

typedef struct __attribute__((packed)) {
    uint8_t ver, op;
    int16_t err;
    uint32_t dropped, sent;
} ack_t;

typedef struct __attribute__((packed)) {
    uint8_t ver, type, channel;
    int8_t rssi;
    uint8_t rate, reserved;
    uint16_t sig_len;
    uint32_t timestamp_us;
    uint16_t cap_len, reserved2;
} frame_hdr_t;

static const char *TAG = "promisc";
static wifi_promiscuous_cb_t s_cb;
static uint32_t s_filter = WIFI_PROMIS_FILTER_MASK_ALL;
static bool s_on, s_ready;
static SemaphoreHandle_t s_ack_sem;
EXT_RAM_BSS_ATTR static StaticSemaphore_t s_ack_mem;
EXT_RAM_BSS_ATTR static ack_t s_ack;
static uint8_t *s_pkt;                 /* PSRAM: one rebuilt wifi_promiscuous_pkt_t, used in the rx context only */

static void on_ack(uint32_t id, const uint8_t *data, size_t len, void *ctx)
{
    (void)id; (void)ctx;
    if (len < sizeof(ack_t)) return;
    memcpy(&s_ack, data, sizeof(s_ack));
    xSemaphoreGive(s_ack_sem);
}

static void on_data(uint32_t id, const uint8_t *data, size_t len, void *ctx)
{
    (void)id; (void)ctx;
    wifi_promiscuous_cb_t cb = s_cb;
    if (!cb || !s_on || len < sizeof(frame_hdr_t) || !s_pkt) return;
    frame_hdr_t h;
    memcpy(&h, data, sizeof(h));
    if (h.ver != LS_PROMISC_VER || sizeof(h) + h.cap_len > len || h.cap_len > SNAP) return;
    wifi_promiscuous_pkt_t *p = (wifi_promiscuous_pkt_t *)s_pkt;
    memset(&p->rx_ctrl, 0, sizeof(p->rx_ctrl));
    p->rx_ctrl.rssi = h.rssi;
    p->rx_ctrl.rate = h.rate;
    p->rx_ctrl.channel = h.channel;
    p->rx_ctrl.timestamp = h.timestamp_us;
    p->rx_ctrl.sig_len = h.cap_len + 4u;   /* what was kept, plus the FCS readers strip */
    memcpy(p->payload, data + sizeof(h), h.cap_len);
    cb(p, (wifi_promiscuous_pkt_type_t)h.type);
}

static esp_err_t ready(void)
{
    if (s_ready) return ESP_OK;
    if (!s_ack_sem) s_ack_sem = xSemaphoreCreateBinaryStatic(&s_ack_mem);
    if (!s_pkt) s_pkt = heap_caps_calloc(1, sizeof(wifi_promiscuous_pkt_t) + SNAP + 8, MALLOC_CAP_SPIRAM);
    if (!s_ack_sem || !s_pkt) return ESP_ERR_NO_MEM;
    esp_err_t e = esp_hosted_register_custom_callback(LS_PROMISC_ACK, on_ack, NULL);
    if (e == ESP_OK) e = esp_hosted_register_custom_callback(LS_PROMISC_DATA, on_data, NULL);
    s_ready = e == ESP_OK;
    return e;
}

static esp_err_t ctrl(uint8_t op, bool enable, uint8_t channel)
{
    esp_err_t e = ready();
    if (e != ESP_OK) return e;
    ctrl_t c = {.ver = LS_PROMISC_VER, .op = op, .enable = enable, .channel = channel,
                .filter_mask = s_filter, .snap = SNAP, .max_per_s = MAX_PER_S};
    xSemaphoreTake(s_ack_sem, 0);
    e = esp_hosted_send_custom_data(LS_PROMISC_CTRL, (const uint8_t *)&c, sizeof(c));
    if (e != ESP_OK) return e;
    if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(ACK_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "the coprocessor did not answer: its firmware has no monitor forwarder");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ack.err;
}

esp_err_t esp_wifi_remote_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb)
{
    s_cb = cb;
    return ESP_OK;
}

esp_err_t esp_wifi_remote_set_promiscuous_filter(const wifi_promiscuous_filter_t *filter)
{
    s_filter = filter && filter->filter_mask ? filter->filter_mask : WIFI_PROMIS_FILTER_MASK_ALL;
    return s_on ? ctrl(OP_SET, true, 0) : ESP_OK;
}

esp_err_t esp_wifi_remote_get_promiscuous_filter(wifi_promiscuous_filter_t *filter)
{
    if (!filter) return ESP_ERR_INVALID_ARG;
    filter->filter_mask = s_filter;
    return ESP_OK;
}

esp_err_t esp_wifi_remote_set_promiscuous(bool en)
{
    if (!en && !s_on) return ESP_OK;
    const esp_err_t e = ctrl(OP_SET, en, 0);
    if (e == ESP_OK || !en) s_on = en && e == ESP_OK;
    if (e == ESP_OK) ESP_LOGI(TAG, "monitor %s (coprocessor: %u sent, %u dropped)", en ? "on" : "off",
                              (unsigned)s_ack.sent, (unsigned)s_ack.dropped);
    return en ? e : ESP_OK;
}

esp_err_t esp_wifi_remote_get_promiscuous(bool *en)
{
    if (!en) return ESP_ERR_INVALID_ARG;
    *en = s_on;
    return ESP_OK;
}

/* c6 mon [seconds]: monitor mode for a while, counting what arrives. A
   check that the coprocessor forwards frames, independent of any app. */
static volatile uint32_t s_t_mgmt, s_t_ctrl, s_t_data, s_t_misc, s_t_bytes;
static volatile int8_t s_t_best = -128;
static volatile uint8_t s_t_ch;
static void test_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *p = buf;
    if (type == WIFI_PKT_MGMT) s_t_mgmt++; else if (type == WIFI_PKT_CTRL) s_t_ctrl++;
    else if (type == WIFI_PKT_DATA) s_t_data++; else s_t_misc++;
    s_t_bytes += p->rx_ctrl.sig_len;
    if (p->rx_ctrl.rssi > s_t_best) { s_t_best = p->rx_ctrl.rssi; s_t_ch = p->rx_ctrl.channel; }
}
int ls_hosted_promisc_test(int secs)
{
    if (secs < 1 || secs > 60) secs = 5;
    s_t_mgmt = s_t_ctrl = s_t_data = s_t_misc = s_t_bytes = 0; s_t_best = -128; s_t_ch = 0;
    wifi_promiscuous_cb_t old = s_cb;
    s_cb = test_rx;
    esp_err_t e = esp_wifi_remote_set_promiscuous(true);
    if (e != ESP_OK) {
        s_cb = old;
        printf("c6 mon: monitor not available: %s%s\n", esp_err_to_name(e),
               e == ESP_ERR_NOT_SUPPORTED ? " (coprocessor firmware has no forwarder)" : "");
        return 1;
    }
    vTaskDelay(pdMS_TO_TICKS(secs * 1000));
    esp_wifi_remote_set_promiscuous(false);
    s_cb = old;
    printf("c6 mon: %d s: mgmt %u, ctrl %u, data %u, other %u, %u bytes; strongest %d dBm on ch %u; coprocessor sent %u dropped %u\n",
           secs, (unsigned)s_t_mgmt, (unsigned)s_t_ctrl, (unsigned)s_t_data, (unsigned)s_t_misc, (unsigned)s_t_bytes,
           s_t_best, (unsigned)s_t_ch, (unsigned)s_ack.sent, (unsigned)s_ack.dropped);
    return 0;
}

/* Called once at boot so this file is linked and its calls replace the weak stubs. */
void ls_hosted_promisc_link(void) {}
