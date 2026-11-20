/* LakeShark: Wi-Fi monitor frames to the host over esp_hosted peer data.

   The host asks with a CTRL message (on/off, frame-type filter, snap length,
   frames per second, channel) and gets an ACK back; while on, each frame the
   radio hears goes to the host as one DATA message. Frames are copied in the
   Wi-Fi task and sent from a task of our own, so a slow link drops frames
   (counted) instead of holding up the radio. Receive only: nothing is sent
   on the air from here. */
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_hosted_peer_data.h"

#define LS_PROMISC_CTRL 0x4C530101u   /* host -> coprocessor */
#define LS_PROMISC_ACK  0x4C530102u   /* coprocessor -> host */
#define LS_PROMISC_DATA 0x4C530103u   /* coprocessor -> host */
#define LS_PROMISC_VER  1

enum { OP_PING = 0, OP_SET = 1, OP_CHANNEL = 2 };

typedef struct __attribute__((packed)) {
    uint8_t ver, op, enable, channel;
    uint32_t filter_mask;               /* WIFI_PROMIS_FILTER_MASK_* */
    uint16_t snap;                      /* bytes of each frame kept */
    uint16_t max_per_s;                 /* frames a second sent, 0 = 400 */
} ctrl_t;

typedef struct __attribute__((packed)) {
    uint8_t ver, op;
    int16_t err;
    uint32_t dropped, sent;
} ack_t;

typedef struct __attribute__((packed)) {
    uint8_t ver, type;                  /* wifi_promiscuous_pkt_type_t */
    uint8_t channel;
    int8_t rssi;
    uint8_t rate, reserved;
    uint16_t sig_len;                   /* on air, FCS included */
    uint32_t timestamp_us;
    uint16_t cap_len, reserved2;        /* bytes that follow */
} frame_hdr_t;

static const char *TAG = "ls_promisc";
static QueueHandle_t s_q;
static volatile bool s_on;
static uint16_t s_snap = 256, s_rate = 400;
static uint32_t s_dropped, s_sent;
static int64_t s_window_us;
static uint32_t s_window_n;

static void ack(uint8_t op, esp_err_t e)
{
    ack_t a = {.ver = LS_PROMISC_VER, .op = op, .err = (int16_t)e, .dropped = s_dropped, .sent = s_sent};
    esp_hosted_send_custom_data(LS_PROMISC_ACK, (const uint8_t *)&a, sizeof(a));
}

static void rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_on || !buf) return;
    const int64_t now = esp_timer_get_time();
    if (now - s_window_us >= 1000000) { s_window_us = now; s_window_n = 0; }
    if (s_window_n >= s_rate) { s_dropped++; return; }
    const wifi_promiscuous_pkt_t *p = buf;
    unsigned len = p->rx_ctrl.sig_len;
    if (len > s_snap) len = s_snap;
    frame_hdr_t *h = malloc(sizeof(*h) + len);
    if (!h) { s_dropped++; return; }
    *h = (frame_hdr_t){.ver = LS_PROMISC_VER, .type = (uint8_t)type, .channel = (uint8_t)p->rx_ctrl.channel,
                       .rssi = (int8_t)p->rx_ctrl.rssi, .rate = (uint8_t)p->rx_ctrl.rate,
                       .sig_len = (uint16_t)p->rx_ctrl.sig_len, .timestamp_us = (uint32_t)p->rx_ctrl.timestamp,
                       .cap_len = (uint16_t)len};
    memcpy(h + 1, p->payload, len);
    if (xQueueSend(s_q, &h, 0) != pdTRUE) { free(h); s_dropped++; return; }
    s_window_n++;
}

static void sender(void *arg)
{
    (void)arg;
    for (;;) {
        frame_hdr_t *h;
        if (xQueueReceive(s_q, &h, portMAX_DELAY) != pdTRUE) continue;
        if (s_on && esp_hosted_send_custom_data(LS_PROMISC_DATA, (const uint8_t *)h, sizeof(*h) + h->cap_len) == ESP_OK)
            s_sent++;
        else
            s_dropped++;
        free(h);
    }
}

static void on_ctrl(uint32_t id, const uint8_t *data, size_t len, void *ctx)
{
    (void)id; (void)ctx;
    if (len < sizeof(ctrl_t)) return;
    ctrl_t c;
    memcpy(&c, data, sizeof(c));
    esp_err_t e = ESP_OK;
    if (c.op == OP_SET) {
        if (c.enable) {
            s_snap = c.snap ? (c.snap > 1600 ? 1600 : c.snap) : 256;
            s_rate = c.max_per_s ? c.max_per_s : 400;
            wifi_promiscuous_filter_t f = {.filter_mask = c.filter_mask ? c.filter_mask : WIFI_PROMIS_FILTER_MASK_ALL};
            e = esp_wifi_set_promiscuous_rx_cb(rx);
            if (e == ESP_OK) e = esp_wifi_set_promiscuous_filter(&f);
            if (e == ESP_OK) { s_dropped = s_sent = 0; s_on = true; e = esp_wifi_set_promiscuous(true); }
            if (e != ESP_OK) s_on = false;
        } else {
            s_on = false;
            e = esp_wifi_set_promiscuous(false);
            esp_wifi_set_promiscuous_rx_cb(NULL);
        }
        ESP_LOGI(TAG, "monitor %s: %s", c.enable ? "on" : "off", esp_err_to_name(e));
    } else if (c.op == OP_CHANNEL) {
        e = c.channel ? esp_wifi_set_channel(c.channel, WIFI_SECOND_CHAN_NONE) : ESP_ERR_INVALID_ARG;
    }
    ack(c.op, e);
}

void ls_promisc_init(void)
{
    s_q = xQueueCreate(48, sizeof(frame_hdr_t *));
    if (!s_q || xTaskCreate(sender, "ls_promisc", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the monitor forwarder");
        return;
    }
    esp_err_t e = esp_hosted_register_custom_callback(LS_PROMISC_CTRL, on_ctrl, NULL);
    ESP_LOGI(TAG, "LakeShark monitor forwarder v%d: %s", LS_PROMISC_VER, esp_err_to_name(e));
}
