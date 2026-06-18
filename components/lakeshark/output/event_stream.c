
#include "event_stream.h"
#include "event_bus.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define STREAM_LINE_MAX     384
#define STREAM_QUEUE_DEPTH  16
#define STREAM_TX_STACK     3072
#define STREAM_TX_PRIO      1
#define STREAM_TX_CORE      0
#define NVS_NS              "sdr-tool"
#define NVS_KEY_ENABLED     "cartotui_en"

static const char *TAG = "event_stream";

typedef struct {
    uint16_t len;
    char     line[STREAM_LINE_MAX];
} stream_msg_t;

static volatile bool     s_enabled = false;
static QueueHandle_t     s_queue   = NULL;
static TaskHandle_t      s_tx_task = NULL;
static volatile uint32_t s_dropped = 0;

static void enqueue_line(const char *body, int body_len)
{
    if (!s_enabled || body_len <= 0 || !s_queue) return;

    stream_msg_t msg;
    if (body_len >= (int)sizeof(msg.line)) body_len = (int)sizeof(msg.line) - 1;
    memcpy(msg.line, body, body_len);
    msg.len = (uint16_t)body_len;

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        s_dropped++;
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    stream_msg_t msg;
    uint32_t reported_drops = 0;

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        if (!s_enabled) continue;

        fwrite(msg.line, 1, msg.len, stdout);
        fputc('\n', stdout);
        fflush(stdout);

        uint32_t d = s_dropped;
        if (d - reported_drops >= 64) {
            reported_drops = d;
            ESP_LOGW(TAG, "CartoTUI feed dropped %lu lines (console saturated)",
                     (unsigned long)d);
        }
    }
}

static void emit_contact(const event_t *e)
{
    char buf[STREAM_LINE_MAX];
    const evt_contact_t *c = &e->u.contact;
    int n = snprintf(buf, sizeof(buf),
        "{\"t\":%lld,\"k\":\"%s\",\"app\":\"%s\","
        "\"icao\":\"%06lX\",\"cs\":\"%s\","
        "\"alt\":%d,\"vel\":%d,\"hdg\":%d,\"vs\":%d,"
        "\"lat\":%.5f,\"lon\":%.5f,\"pos\":%s,\"shaky\":%s}",
        (long long)e->ts_us, evt_kind_name(e->kind), e->app,
        (unsigned long)c->icao, c->callsign,
        c->altitude, c->velocity, c->heading, c->vert_rate,
        c->lat, c->lon,
        c->pos_valid ? "true" : "false",
        c->crc_shaky ? "true" : "false");
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    enqueue_line(buf, n);
}

static void on_event(const event_t *e, void *user)
{
    (void)user;
    if (!e || !s_enabled) return;
    switch (e->kind) {
    case EVT_CONTACT_NEW:
    case EVT_CONTACT_CONFIRMED:
    case EVT_CONTACT_POSITION:
    case EVT_CONTACT_LOST:

        emit_contact(e);
        break;
    default:
        break;
    }
}

static void persist_enabled(bool en)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_ENABLED, en ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static bool load_enabled(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, NVS_KEY_ENABLED, &v);
    nvs_close(h);
    return v != 0;
}

void event_stream_set_enabled(bool en)
{
    s_enabled = en;
    persist_enabled(en);
    if (en) {

        const char *banner = "{\"k\":\"STREAM_INIT\",\"feed\":\"cartotui\"}";
        enqueue_line(banner, (int)strlen(banner));
    }
    ESP_LOGI(TAG, "CartoTUI feed %s (console JSONL)", en ? "ENABLED" : "disabled");
}

bool event_stream_enabled(void) { return s_enabled; }

void event_stream_init(void)
{
    if (!s_queue) {
        s_queue = xQueueCreate(STREAM_QUEUE_DEPTH, sizeof(stream_msg_t));
    }
    if (s_queue && !s_tx_task) {
        xTaskCreatePinnedToCore(tx_task, "cartotui_tx", STREAM_TX_STACK,
                                NULL, STREAM_TX_PRIO, &s_tx_task, STREAM_TX_CORE);
    }
    event_bus_subscribe(on_event, NULL);
    s_enabled = load_enabled();
}
