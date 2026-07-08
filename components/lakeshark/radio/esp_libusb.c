
#include "usb/usb_host.h"
#include "esp_log.h"
#include "esp_libusb.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static class_adsb_dev *adsbdev;

void init_adsb_dev(void)
{
    if (adsbdev) return;

    adsbdev = calloc(1, sizeof(class_adsb_dev));
    adsbdev->is_adsb = true;
    adsbdev->response_buf = calloc(256, sizeof(uint8_t));
    adsbdev->done_sem = xSemaphoreCreateBinary();

    esp_err_t r = usb_host_transfer_alloc(256, 0, &adsbdev->transfer);
    if (r != ESP_OK) {
        ESP_LOGE(TAG_ADSB, "Failed to allocate control transfer");
    }
}

void bulk_transfer_read_cb(usb_transfer_t *transfer)
{
    adsbdev->is_success = (transfer->status == 0);
    adsbdev->bytes_transferred = transfer->actual_num_bytes;
    xSemaphoreGive(adsbdev->done_sem);
}

void transfer_read_cb(usb_transfer_t *transfer)
{
    for (int i = 0; i < transfer->actual_num_bytes; i++) {
        adsbdev->response_buf[i] = transfer->data_buffer[i];
    }
    adsbdev->is_success = (transfer->status == 0);
    adsbdev->bytes_transferred = transfer->actual_num_bytes - sizeof(usb_setup_packet_t);
    xSemaphoreGive(adsbdev->done_sem);
}

#define BULK_XFER_SLOTS 4
static usb_transfer_t   *s_xfer[BULK_XFER_SLOTS]      = {0};
static SemaphoreHandle_t s_xfer_sem[BULK_XFER_SLOTS]  = {0};
static volatile bool     s_xfer_ok[BULK_XFER_SLOTS]   = {0};
static volatile int      s_xfer_bytes[BULK_XFER_SLOTS]= {0};
static int               s_nslots    = 0;
static int               s_read_idx  = 0;
static bool              s_primed    = false;
static size_t            s_xfer_size = 0;
static usb_device_handle_t s_bulk_dev = NULL;
static unsigned char     s_bulk_ep   = 0;

void esp_libusb_stream_stop(void);

void esp_libusb_bulk_teardown(void)
{
    if (!s_xfer[0] && !s_xfer_sem[0]) return;
    if (s_bulk_dev) {
        usb_host_endpoint_halt(s_bulk_dev, s_bulk_ep);
        usb_host_endpoint_flush(s_bulk_dev, s_bulk_ep);
        usb_host_endpoint_clear(s_bulk_dev, s_bulk_ep);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < BULK_XFER_SLOTS; i++) {
        if (s_xfer[i])     { usb_host_transfer_free(s_xfer[i]); s_xfer[i] = NULL; }
        if (s_xfer_sem[i]) { vSemaphoreDelete(s_xfer_sem[i]);   s_xfer_sem[i] = NULL; }
    }
    s_nslots = 0; s_read_idx = 0; s_primed = false;
}

static void bulk_transfer_read_cb_pp(usb_transfer_t *transfer)
{
    int slot = (int)(intptr_t)transfer->context;
    if (slot < 0 || slot >= BULK_XFER_SLOTS) return;
    s_xfer_ok[slot]    = (transfer->status == 0);
    s_xfer_bytes[slot] = transfer->actual_num_bytes;
    xSemaphoreGive(s_xfer_sem[slot]);
}

extern int64_t esp_timer_get_time(void);

static int bulk_xfer_init(class_driver_t *driver_obj, int length,
                          unsigned char endpoint)
{
    esp_libusb_stream_stop();
    s_bulk_dev = driver_obj->dev_hdl;
    s_bulk_ep  = endpoint;
    s_xfer_size = usb_round_up_to_mps(length, 512);
    s_nslots = 0; s_read_idx = 0; s_primed = false;
    for (int i = 0; i < BULK_XFER_SLOTS; i++) {
        s_xfer_sem[i] = xSemaphoreCreateBinary();
        if (!s_xfer_sem[i]) break;
        if (usb_host_transfer_alloc(s_xfer_size, 0, &s_xfer[i]) != ESP_OK) {
            vSemaphoreDelete(s_xfer_sem[i]);
            s_xfer_sem[i] = NULL;
            break;
        }
        s_xfer[i]->num_bytes        = s_xfer_size;
        s_xfer[i]->device_handle    = driver_obj->dev_hdl;
        s_xfer[i]->bEndpointAddress = endpoint;
        s_xfer[i]->callback         = bulk_transfer_read_cb_pp;
        s_xfer[i]->context          = (void *)(intptr_t)i;
        s_nslots++;
    }
    if (s_nslots == 0) {
        ESP_LOGE(TAG_ADSB, "bulk_xfer_init: no transfer slots (out of DMA)");
        return -1;
    }
    ESP_LOGI(TAG_ADSB, "bulk: %d slots x %u B (~%d ms in-flight buffer)",
             s_nslots, (unsigned)s_xfer_size,
             (int)((s_nslots * s_xfer_size) / 1920));
    return 0;
}

static esp_err_t bulk_submit(usb_transfer_t *x, unsigned int timeout)
{
    x->timeout_ms = timeout;
    esp_err_t r = ESP_FAIL;
    for (int a = 0; a < 3; a++) {
        r = usb_host_transfer_submit(x);
        if (r == ESP_OK) break;
        if (r == 0x10C) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        break;
    }
    return r;
}

extern void app_request_recover(void);

static int s_recover_fails = 0;
static int s_teardowns     = 0;

static void bulk_recover(class_driver_t *driver_obj, unsigned char endpoint)
{
    usb_host_endpoint_clear(driver_obj->dev_hdl, endpoint);
    usb_host_endpoint_halt(driver_obj->dev_hdl, endpoint);
    usb_host_endpoint_flush(driver_obj->dev_hdl, endpoint);
    usb_host_endpoint_clear(driver_obj->dev_hdl, endpoint);
    for (int i = 0; i < s_nslots; i++)
        xSemaphoreTake(s_xfer_sem[i], pdMS_TO_TICKS(20));
    s_primed = false;
    s_read_idx = 0;

    if (++s_recover_fails >= 4) {
        s_recover_fails = 0;
        if (++s_teardowns >= 3) {
            s_teardowns = 0;
            ESP_LOGE(TAG_ADSB, "bulk: pipe unrecoverable -> requesting RTL device re-open");
            app_request_recover();
        } else {
            ESP_LOGW(TAG_ADSB, "bulk: repeated recovery failures -> full teardown + reinit");
            esp_libusb_bulk_teardown();
        }
    }
}

int esp_libusb_bulk_transfer(class_driver_t *driver_obj, unsigned char endpoint,
                             unsigned char *data, int length, int *transferred,
                             unsigned int timeout)
{
    if (!s_xfer[0]) {
        if (bulk_xfer_init(driver_obj, length, endpoint) != 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            return -1;
        }
    }

    if (!s_primed) {
        s_read_idx = 0;
        for (int i = 0; i < s_nslots; i++) {
            xSemaphoreTake(s_xfer_sem[i], 0);
            if (bulk_submit(s_xfer[i], timeout) != ESP_OK) {
                static int64_t le = 0; int64_t now = esp_timer_get_time();
                if (now - le > 1000000LL) { ESP_LOGE(TAG_ADSB, "bulk prime failed (slot %d)", i); le = now; }
                bulk_recover(driver_obj, endpoint);
                vTaskDelay(pdMS_TO_TICKS(50));
                return -1;
            }
        }
        s_primed = true;
    }

    int idx = s_read_idx;

    if (xSemaphoreTake(s_xfer_sem[idx], pdMS_TO_TICKS(timeout + 500)) != pdTRUE) {
        ESP_LOGE(TAG_ADSB, "bulk timeout (slot %d)", idx);
        bulk_recover(driver_obj, endpoint);
        return -1;
    }
    if (!s_xfer_ok[idx]) {
        ESP_LOGW(TAG_ADSB, "bulk STALL/fail (slot %d)", idx);
        bulk_recover(driver_obj, endpoint);
        vTaskDelay(pdMS_TO_TICKS(20));
        return -1;
    }

    int done_bytes = s_xfer_bytes[idx];
    *transferred = done_bytes;
    memcpy(data, s_xfer[idx]->data_buffer, done_bytes);
    s_recover_fails = 0;
    s_teardowns     = 0;

    xSemaphoreTake(s_xfer_sem[idx], 0);
    if (bulk_submit(s_xfer[idx], timeout) != ESP_OK) {
        static int64_t lr = 0; int64_t now = esp_timer_get_time();
        if (now - lr > 1000000LL) { ESP_LOGE(TAG_ADSB, "bulk repost failed (slot %d)", idx); lr = now; }
        bulk_recover(driver_obj, endpoint);
        return 0;
    }
    s_read_idx = (idx + 1) % s_nslots;
    return 0;
}

#define STREAM_XFER_NUM   8
#define STREAM_XFER_LEN   16384
#define STREAM_RING_SIZE  (256u * 1024u)

static uint8_t           *s_sring;
static volatile uint32_t  s_shead, s_stail;
static usb_transfer_t    *s_sxfer[STREAM_XFER_NUM];
static usb_device_handle_t s_sdev;
static unsigned char      s_sep;
static volatile bool      s_streaming;
static uint64_t           s_sdropped;
static QueueHandle_t      s_squeue;
static TaskHandle_t       s_spump;

static void IRAM_ATTR stream_push(const uint8_t *buf, uint32_t len)
{
    uint32_t head  = s_shead;
    uint32_t space = STREAM_RING_SIZE - (head - s_stail);
    if (len > space) { s_sdropped += len; return; }
    uint32_t off   = head % STREAM_RING_SIZE;
    uint32_t first = STREAM_RING_SIZE - off;
    if (first >= len) {
        memcpy(s_sring + off, buf, len);
    } else {
        memcpy(s_sring + off, buf, first);
        memcpy(s_sring, buf + first, len - first);
    }
    s_shead = head + len;
}

static void IRAM_ATTR stream_xfer_cb(usb_transfer_t *t)
{
    if (t->status == 0 && t->actual_num_bytes > 0)
        stream_push(t->data_buffer, (uint32_t)t->actual_num_bytes);
    int slot = (int)(intptr_t)t->context;
    if (s_streaming && s_squeue)
        xQueueSend(s_squeue, &slot, 0);
}

static bool stream_reprime(void)
{
    if (!s_sdev) return false;
    usb_host_endpoint_halt(s_sdev, s_sep);
    usb_host_endpoint_flush(s_sdev, s_sep);
    usb_host_endpoint_clear(s_sdev, s_sep);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (s_squeue) xQueueReset(s_squeue);

    int posted = 0;
    for (int i = 0; i < STREAM_XFER_NUM; i++) {
        if (!s_sxfer[i]) continue;
        s_sxfer[i]->device_handle    = s_sdev;
        s_sxfer[i]->bEndpointAddress = s_sep;
        if (usb_host_transfer_submit(s_sxfer[i]) == ESP_OK) posted++;
    }
    return posted > 0;
}

static void stream_pump_task(void *arg)
{
    (void)arg;
    uint32_t last_head  = s_shead;
    int64_t  last_log   = esp_timer_get_time();
    int      stall_secs = 0;
    while (s_streaming) {
        int slot;
        if (xQueueReceive(s_squeue, &slot, pdMS_TO_TICKS(50)) == pdTRUE &&
            s_streaming && slot >= 0 && slot < STREAM_XFER_NUM && s_sxfer[slot]) {
            esp_err_t r = usb_host_transfer_submit(s_sxfer[slot]);
            if (r == 0x10C) {
                vTaskDelay(pdMS_TO_TICKS(2));
                xQueueSend(s_squeue, &slot, 0);
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - last_log >= 1000000) {
            uint32_t bytes = s_shead - last_head;
            if (s_streaming && bytes == 0) {
                stall_secs++;
                ESP_LOGW(TAG_ADSB, "stream stalled %ds, re-priming pipe (dropped=%llu)",
                         stall_secs, (unsigned long long)s_sdropped);
                stream_reprime();
                if (stall_secs >= 5) {
                    ESP_LOGE(TAG_ADSB, "stream pump dead %ds -> requesting RTL device re-open",
                             stall_secs);
                    stall_secs = 0;
                    app_request_recover();
                }
            } else {
                stall_secs = 0;
                ESP_LOGD(TAG_ADSB, "stream throughput: %u B/s (%.2f MB/s), dropped=%llu",
                         (unsigned)bytes, bytes / 1e6, (unsigned long long)s_sdropped);
            }
            last_head = s_shead; last_log = now;
        }
    }
    s_spump = NULL;
    vTaskDelete(NULL);
}

int esp_libusb_stream_start(class_driver_t *driver_obj, unsigned char endpoint)
{

    esp_libusb_bulk_teardown();
    if (s_streaming) esp_libusb_stream_stop();

    if (!s_sring) {
        s_sring = heap_caps_malloc(STREAM_RING_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_sring) { ESP_LOGE(TAG_ADSB, "stream ring alloc failed"); return -1; }
    }
    if (!s_squeue) s_squeue = xQueueCreate(STREAM_XFER_NUM * 2, sizeof(int));
    if (!s_squeue) { ESP_LOGE(TAG_ADSB, "stream queue alloc failed"); return -1; }
    xQueueReset(s_squeue);
    s_shead = s_stail = 0; s_sdropped = 0;
    s_sdev = driver_obj->dev_hdl; s_sep = endpoint;
    s_streaming = true;

    if (!s_spump)
        xTaskCreatePinnedToCore(stream_pump_task, "rtl_pump", 4096, NULL, 12, &s_spump, 1);

    int posted = 0;
    for (int i = 0; i < STREAM_XFER_NUM; i++) {
        s_sxfer[i] = NULL;
        if (usb_host_transfer_alloc(STREAM_XFER_LEN, 0, &s_sxfer[i]) != ESP_OK) { s_sxfer[i] = NULL; break; }
        s_sxfer[i]->num_bytes        = STREAM_XFER_LEN;
        s_sxfer[i]->device_handle    = s_sdev;
        s_sxfer[i]->bEndpointAddress = endpoint;
        s_sxfer[i]->callback         = stream_xfer_cb;
        s_sxfer[i]->context          = (void *)(intptr_t)i;
        if (usb_host_transfer_submit(s_sxfer[i]) != ESP_OK) {
            usb_host_transfer_free(s_sxfer[i]); s_sxfer[i] = NULL; break;
        }
        posted++;
    }
    if (posted == 0) { s_streaming = false; ESP_LOGE(TAG_ADSB, "stream: 0 transfers posted"); return -1; }
    ESP_LOGI(TAG_ADSB, "stream: %d x %d B posted, %u KB PSRAM IQ ring, pump up",
             posted, STREAM_XFER_LEN, (unsigned)(STREAM_RING_SIZE / 1024));
    return 0;
}

void esp_libusb_stream_stop(void)
{
    if (!s_streaming) return;
    s_streaming = false;

    for (int i = 0; i < 20 && s_spump; i++) vTaskDelay(pdMS_TO_TICKS(5));
    usb_host_endpoint_halt(s_sdev, s_sep);
    usb_host_endpoint_flush(s_sdev, s_sep);
    usb_host_endpoint_clear(s_sdev, s_sep);
    vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < STREAM_XFER_NUM; i++) {
        if (s_sxfer[i]) { usb_host_transfer_free(s_sxfer[i]); s_sxfer[i] = NULL; }
    }
}

int esp_libusb_stream_read(uint8_t *dst, int max)
{
    uint32_t tail  = s_stail;
    uint32_t avail = s_shead - tail;
    if (avail == 0) return 0;
    if ((uint32_t)max > avail) max = (int)avail;
    uint32_t off   = tail % STREAM_RING_SIZE;
    uint32_t first = STREAM_RING_SIZE - off;
    if (first >= (uint32_t)max) {
        memcpy(dst, s_sring + off, (size_t)max);
    } else {
        memcpy(dst, s_sring + off, first);
        memcpy(dst + first, s_sring, (size_t)max - first);
    }
    s_stail = tail + (uint32_t)max;
    return max;
}

void     esp_libusb_stream_reset(void) { s_stail = s_shead; }
uint32_t esp_libusb_stream_avail(void) { return s_shead - s_stail; }

int esp_libusb_control_transfer(class_driver_t *driver_obj, uint8_t bm_req_type, uint8_t b_request, uint16_t wValue, uint16_t wIndex, unsigned char *data, uint16_t wLength, unsigned int timeout)
{
    if (!adsbdev || !adsbdev->transfer) return -1;

    size_t sizePacket = sizeof(usb_setup_packet_t) + wLength;

    USB_SETUP_PACKET_INIT_CONTROL((usb_setup_packet_t *)adsbdev->transfer->data_buffer,
                                  bm_req_type, b_request, wValue, wIndex, wLength);

    adsbdev->transfer->num_bytes = sizePacket;
    adsbdev->transfer->device_handle = driver_obj->dev_hdl;
    adsbdev->transfer->timeout_ms = timeout;
    adsbdev->transfer->context = (void *)&driver_obj;
    adsbdev->transfer->callback = transfer_read_cb;

    if (bm_req_type == CTRL_OUT && data && wLength > 0) {
        for (uint8_t i = 0; i < wLength; i++) {
            adsbdev->transfer->data_buffer[sizeof(usb_setup_packet_t) + i] = data[i];
        }
    }

    xSemaphoreTake(adsbdev->done_sem, 0);

    esp_err_t r = usb_host_transfer_submit_control(driver_obj->client_hdl, adsbdev->transfer);
    if (r != ESP_OK) {
        ESP_LOGE(TAG_ADSB, "libusb_control_transfer failed to submit: %d", r);
        vTaskDelay(pdMS_TO_TICKS(50));
        return -1;
    }

    if (xSemaphoreTake(adsbdev->done_sem, pdMS_TO_TICKS(timeout + 500)) != pdTRUE) {
        ESP_LOGE(TAG_ADSB, "Control transfer timed out");
        return -1;
    }

    if (!adsbdev->is_success) {
        ESP_LOGW(TAG_ADSB, "libusb_control_transfer STALL/Fail");
        vTaskDelay(pdMS_TO_TICKS(50));
        return -1;
    }

    if (bm_req_type == CTRL_IN && data && wLength > 0) {
        for (uint8_t i = 0; i < wLength; i++) {
            data[i] = adsbdev->response_buf[sizeof(usb_setup_packet_t) + i];
        }
    }

    return adsbdev->bytes_transferred;
}

void esp_libusb_get_string_descriptor_ascii(const usb_str_desc_t *str_desc, char *str)
{
    if (str_desc == NULL) {
        return;
    }

    for (int i = 0; i < str_desc->bLength / 2; i++) {
        str[i] = (char)str_desc->wData[i];
    }
}
