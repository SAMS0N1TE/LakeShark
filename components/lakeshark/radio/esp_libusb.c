/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "usb/usb_host.h"
#include "esp_log.h"
#include "esp_libusb_private.h"
#include "rtl_adapter_private.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
/*LS-415*/
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

static class_adsb_dev *adsbdev;

/*LS-813  One control transfer at a time.

   esp_libusb_control_transfer drives a SINGLE shared usb_transfer_t and a
   single done_sem, and clears that semaphore on entry. Two callers at once
   and the second overwrites the first's setup packet in place, then the
   first's completion satisfies the second's wait - a STALL, or a tuner
   register written with someone else's value, which reads as flaky hardware.

   LS-180 routes every IQ-app tune and gain through its session-owning RX task,
   but lifecycle and adapter recovery can still issue control transfers. Keep
   the endpoint guard as defense in depth rather than relying on every future
   caller to preserve that routing. */
static SemaphoreHandle_t s_ctl_mux;

void init_adsb_dev(void)
{
    if (adsbdev) return;

    adsbdev = calloc(1, sizeof(class_adsb_dev));
    adsbdev->is_adsb = true;
    adsbdev->response_buf = calloc(256, sizeof(uint8_t));
    adsbdev->done_sem = xSemaphoreCreateBinary();
    if (!s_ctl_mux) s_ctl_mux = xSemaphoreCreateMutex();

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
    if (!s_xfer[0] && !s_xfer_sem[0]) {
        s_bulk_dev = NULL;
        s_bulk_ep = 0;
        return;
    }
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
    s_bulk_dev = NULL; s_bulk_ep = 0;
}

void esp_libusb_bulk_teardown_for(class_driver_t *driver_obj)
{
    if (driver_obj && s_bulk_dev == driver_obj->dev_hdl)
        esp_libusb_bulk_teardown();
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

static int s_recover_fails = 0;
static int s_teardowns     = 0;

static void bulk_recover(class_driver_t *driver_obj, unsigned char endpoint)
{

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
            ESP_LOGE(TAG_ADSB, "bulk: pipe unrecoverable -> reporting RTL endpoint fault");
            rtl_adapter_note_transport_fault();
        } else {
            ESP_LOGW(TAG_ADSB, "bulk: repeated recovery failures -> full teardown + reinit");
            esp_libusb_bulk_teardown();
        }
    }
}

/*LS-406*/ /*LS-415*/
static volatile uint32_t s_total_rx;
static volatile int64_t  s_bulk_last_us;

/*LS-415*/
bool esp_libusb_bulk_active(void)
{
    int64_t t = s_bulk_last_us;
    return t != 0 && (esp_timer_get_time() - t) < 1000000LL;
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
    /*LS-415*/
    s_total_rx    += (uint32_t)done_bytes;
    s_bulk_last_us = esp_timer_get_time();
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

#ifndef STREAM_XFER_NUM
#define STREAM_XFER_NUM   16
#endif
#define STREAM_XFER_LEN   16384
#define STREAM_RING_SIZE  (256u * 1024u)
#define STREAM_PUMP_STACK_BYTES 4096u
#define STREAM_QUEUE_LEN (STREAM_XFER_NUM * 2u)

static uint8_t           *s_sring;
static volatile uint32_t  s_shead, s_stail;
static usb_transfer_t    *s_sxfer[STREAM_XFER_NUM];
static usb_device_handle_t s_sdev;
static unsigned char      s_sep;
static volatile bool      s_streaming;
static uint64_t           s_sdropped;
static QueueHandle_t      s_squeue;
static DRAM_ATTR StaticQueue_t s_squeue_ctrl;
static DRAM_ATTR uint8_t s_squeue_store[STREAM_QUEUE_LEN * sizeof(int)];
static TaskHandle_t       s_spump;
static DRAM_ATTR StackType_t
    s_spump_stack[STREAM_PUMP_STACK_BYTES / sizeof(StackType_t)]
        __attribute__((aligned(16)));
static DRAM_ATTR StaticTask_t s_spump_tcb;

/*LS-401*/
#define LS_STREAM_JOIN_MS 2000

uint32_t esp_libusb_total_bytes(void) { return s_total_rx; }
bool     esp_libusb_streaming(void)   { return s_streaming; }

/*LS-407*/
void esp_libusb_note_device_gone(usb_device_handle_t device)
{
    /*LS-415*/
    bool bulk_gone = s_bulk_dev == device;
    bool stream_gone = s_sdev == device;
    if (bulk_gone) {
        s_bulk_last_us = 0;
        s_bulk_dev = NULL;
        s_primed = false;
    }
    if (stream_gone) {
        s_streaming = false;
        s_sdev = NULL;
        s_sep = 0;
    }
    /* Handles must be invalidated before cleanup so neither path calls an
     * endpoint API on a vanished device. Transfer objects are still ours. */
    if (stream_gone) esp_libusb_stream_stop();
    if (bulk_gone) esp_libusb_bulk_teardown();
}

/*LS-402*/
static void IRAM_ATTR stream_push(const uint8_t *buf, uint32_t len)
{
    /*LS-406*/
    s_total_rx += len;

    uint32_t head  = s_shead;
    uint32_t tail  = __atomic_load_n(&s_stail, __ATOMIC_ACQUIRE);
    uint32_t space = STREAM_RING_SIZE - (head - tail);
    if (len > space) { s_sdropped += len; return; }
    uint32_t off   = head % STREAM_RING_SIZE;
    uint32_t first = STREAM_RING_SIZE - off;
    if (first >= len) {
        memcpy(s_sring + off, buf, len);
    } else {
        memcpy(s_sring + off, buf, first);
        memcpy(s_sring, buf + first, len - first);
    }
    __atomic_store_n(&s_shead, head + len, __ATOMIC_RELEASE);
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
                /*LS-410*/
                stall_secs++;
                ESP_LOGW(TAG_ADSB, "stream stalled %ds, re-priming pipe (dropped=%llu)",
                         stall_secs, (unsigned long long)s_sdropped);
                stream_reprime();
            } else {
                stall_secs = 0;
                ESP_LOGW(TAG_ADSB, "stream throughput: %u B/s (%.2f MB/s), dropped=%llu",
                         (unsigned)bytes, bytes / 1e6, (unsigned long long)s_sdropped);
            }
            last_head = s_shead; last_log = now;
        }
    }
    /* LS-730: this task owns a fixed internal stack.  Suspend at the terminal
     * point and let stop() observe that kernel state before deleting the TCB;
     * clearing a software handle before the last stack access made immediate
     * reentry capable of reusing a still-running static stack. */
    vTaskSuspend(NULL);
}

static bool stream_pump_join(uint32_t timeout_ms)
{
    TaskHandle_t task = s_spump;
    if (!task) return true;

    uint32_t waited_ms = 0;
    while (eTaskGetState(task) != eSuspended) {
        if (waited_ms >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }
    vTaskDelete(task);
    s_spump = NULL;
    return true;
}

int esp_libusb_stream_start(class_driver_t *driver_obj, unsigned char endpoint)
{
    /* LS-340: RTL and HackRF endpoints may coexist, but the current USB
     * transport has one transfer pool and one PSRAM ring. Refuse a second
     * producer instead of stopping the first radio or mixing its samples. */
    if ((s_streaming && s_sdev != driver_obj->dev_hdl) ||
        (s_bulk_dev && s_bulk_dev != driver_obj->dev_hdl)) {
        ESP_LOGW(TAG_ADSB, "IQ transport busy on another USB radio");
        return ESP_LIBUSB_ERR_BUSY;
    }

    esp_libusb_bulk_teardown_for(driver_obj);
    if (s_streaming) esp_libusb_stream_stop_for(driver_obj);

    if (!s_sring) {
        s_sring = heap_caps_malloc(STREAM_RING_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_sring) {
            ESP_LOGE(TAG_ADSB, "stream ring alloc failed");
            return ESP_LIBUSB_ERR_NO_MEM;
        }
    }
    /* LS-730: cold LCD entry had 27 bytes internal free and no sufficiently
     * large contiguous block, so dynamic queue/TCB/stack allocation failed
     * every 405 ms.  These ISR/task-owned resources are fixed in internal
     * DRAM; FM teardown ordering can no longer decide whether P25 starts. */
    if (!s_squeue)
        s_squeue = xQueueCreateStatic(STREAM_QUEUE_LEN, sizeof(int),
                                      s_squeue_store, &s_squeue_ctrl);
    if (!s_squeue) {
        ESP_LOGE(TAG_ADSB, "stream static queue init failed");
        return ESP_LIBUSB_ERR_NO_MEM;
    }

    /*LS-401*/
    if (!stream_pump_join(200)) {
        ESP_LOGE(TAG_ADSB, "previous rtl_pump has not exited - refusing to start a "
                           "stream that would have no pump to repost transfers");
        return -1;
    }

    xQueueReset(s_squeue);
    s_shead = s_stail = 0; s_sdropped = 0;
    s_sdev = driver_obj->dev_hdl; s_sep = endpoint;
    s_streaming = true;

    s_spump = xTaskCreateStaticPinnedToCore(
        stream_pump_task, "rtl_pump", sizeof(s_spump_stack), NULL, 12,
        s_spump_stack, &s_spump_tcb, 1);
    if (!s_spump) {
        /* LS-1003: reporting success here posted one finite USB window with no
         * consumer to repost it: 16 x 16384 = the hardware's exact 262144-byte
         * plateau.  Refuse the stream before submitting anything, so the app
         * sees a start failure instead of ACTIVE followed by watchdog churn. */
        s_streaming = false;
        s_spump = NULL;
        s_sdev = NULL;
        s_sep = 0;
        ESP_LOGE(TAG_ADSB,
                 "stream: rtl_pump task start failed (internal=%u largest=%u); no transfers posted",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_LIBUSB_ERR_NO_MEM;
    }

    int posted = 0;
    bool transfer_alloc_failed = false;
    for (int i = 0; i < STREAM_XFER_NUM; i++) {
        s_sxfer[i] = NULL;
        if (usb_host_transfer_alloc(STREAM_XFER_LEN, 0, &s_sxfer[i]) != ESP_OK) {
            s_sxfer[i] = NULL;
            transfer_alloc_failed = true;
            break;
        }
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
    if (posted == 0) {
        s_streaming = false;
        ESP_LOGE(TAG_ADSB, "stream: 0 transfers posted (%s)",
                 transfer_alloc_failed ? "out of DMA" : "USB submit failed");
        esp_libusb_stream_stop();
        return transfer_alloc_failed ? ESP_LIBUSB_ERR_NO_MEM : -1;
    }
    ESP_LOGI(TAG_ADSB, "stream: %d x %d B posted, %u KB PSRAM IQ ring, pump up",
             posted, STREAM_XFER_LEN, (unsigned)(STREAM_RING_SIZE / 1024));
    return 0;
}

/*LS-401*/
void esp_libusb_stream_stop(void)
{
    /*LS-407*/
    if (!s_streaming && !s_spump && esp_libusb_stream_slots() == 0) return;
    s_streaming = false;

    bool joined = stream_pump_join(LS_STREAM_JOIN_MS);

    if (s_sdev) {
        usb_host_endpoint_halt(s_sdev, s_sep);
        usb_host_endpoint_flush(s_sdev, s_sep);
        usb_host_endpoint_clear(s_sdev, s_sep);
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    if (!joined) {
        ESP_LOGE(TAG_ADSB, "rtl_pump did not exit within %d ms - leaking %d transfer "
                           "slots rather than freeing buffers the USB stack may still "
                           "be writing to", LS_STREAM_JOIN_MS, esp_libusb_stream_slots());
        for (int i = 0; i < STREAM_XFER_NUM; i++) s_sxfer[i] = NULL;
    } else {
        for (int i = 0; i < STREAM_XFER_NUM; i++) {
            if (s_sxfer[i]) { usb_host_transfer_free(s_sxfer[i]); s_sxfer[i] = NULL; }
        }
    }

    /*LS-403*/
    s_sdev = NULL;
    s_sep  = 0;
}

void esp_libusb_stream_stop_for(class_driver_t *driver_obj)
{
    if (driver_obj && s_sdev == driver_obj->dev_hdl)
        esp_libusb_stream_stop();
}

bool esp_libusb_stream_owned_by(const class_driver_t *driver_obj)
{
    return driver_obj && s_streaming && s_sdev == driver_obj->dev_hdl;
}

/*LS-402*/
int esp_libusb_stream_read(uint8_t *dst, int max)
{
    uint32_t tail  = s_stail;
    uint32_t avail = __atomic_load_n(&s_shead, __ATOMIC_ACQUIRE) - tail;
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
    __atomic_store_n(&s_stail, tail + (uint32_t)max, __ATOMIC_RELEASE);
    return max;
}

int esp_libusb_stream_read_timeout(uint8_t *dst, int max,
                                   uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;
    for (;;) {
        if (!s_streaming) return -1;
        int read_bytes = esp_libusb_stream_read(dst, max);
        if (read_bytes != 0) return read_bytes;
        if (waited_ms >= timeout_ms) return 0;
        vTaskDelay(pdMS_TO_TICKS(1));
        ++waited_ms;
    }
}

void esp_libusb_stream_reset(void)
{
    __atomic_store_n(&s_stail, __atomic_load_n(&s_shead, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);
}

uint32_t esp_libusb_stream_avail(void)
{
    return __atomic_load_n(&s_shead, __ATOMIC_ACQUIRE) - s_stail;
}

uint64_t esp_libusb_stream_dropped(void) { return s_sdropped; }

int esp_libusb_stream_slots(void)
{
    int n = 0;
    for (int i = 0; i < STREAM_XFER_NUM; i++) if (s_sxfer[i]) n++;
    return n;
}

static int control_transfer_locked(class_driver_t *driver_obj, uint8_t bm_req_type, uint8_t b_request, uint16_t wValue, uint16_t wIndex, unsigned char *data, uint16_t wLength, unsigned int timeout)
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

/*LS-813  Public entry: serialise, then run the transfer. The mutex is held
   across submit AND the wait on done_sem - it is the pairing of the two that
   must be atomic, because the shared transfer buffer is in use for that whole
   window. Falls through unguarded if the mutex could not be created, which is
   the old behaviour rather than a hard failure. */
int esp_libusb_control_transfer(class_driver_t *driver_obj, uint8_t bm_req_type,
                                uint8_t b_request, uint16_t wValue, uint16_t wIndex,
                                unsigned char *data, uint16_t wLength, unsigned int timeout)
{
    if (!s_ctl_mux)
        return control_transfer_locked(driver_obj, bm_req_type, b_request,
                                       wValue, wIndex, data, wLength, timeout);

    if (xSemaphoreTake(s_ctl_mux, pdMS_TO_TICKS(timeout + 1000)) != pdTRUE) {
        ESP_LOGE(TAG_ADSB, "control transfer: mutex timeout, request dropped");
        return -1;
    }
    int r = control_transfer_locked(driver_obj, bm_req_type, b_request,
                                    wValue, wIndex, data, wLength, timeout);
    xSemaphoreGive(s_ctl_mux);
    return r;
}
