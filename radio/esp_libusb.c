/*
 * esp_libusb.c — USB transfer shim for RTL-SDR V4 on ESP32-P4 USB host.
 *
 * TWO transfer paths:
 *
 *   1. Control transfer (EP 0) — vendor requests to the RTL2832U / tuner
 *      for init/tune/gain. Infrequent (a few per tune), allocated once
 *      as `adsbdev->transfer`, synchronous from the caller's perspective
 *      via a binary semaphore.
 *
 *   2. Bulk transfer (EP 1) — 32 KB IQ sample reads, called continuously.
 *      DOUBLE-BUFFERED: while the caller processes buffer A, buffer B
 *      is already submitted and filling. This keeps the RTL's USB
 *      endpoint constantly draining. The old single-buffer design left
 *      the endpoint idle between caller returns and next submit, which:
 *        (a) caused RTL FIFO overflow / sample drops at high rates,
 *        (b) triggered frequent EP 0 STALL events during idle gaps
 *            (the hub driver pokes EP0 when bulk activity pauses),
 *            which in turn spammed `esp_rom_printf` from ISR context
 *            and corrupted TUI rendering on UART0.
 *
 *   This file is the ADS-B port of the P25 project's `esp_libusb.c`.
 *   The interfaces (function signatures in esp_libusb.h) are unchanged,
 *   so the class_driver.c bulk-read call site needs NO modifications.
 */
#include "usb/usb_host.h"
#include "esp_log.h"
#include "esp_libusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* ── Control transfer path (EP0) — unchanged from the old file ──────── */

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

/* Kept for API compatibility — referenced by esp_libusb.h. The double-
 * buffered bulk path below uses its own internal callback
 * (bulk_transfer_read_cb_pp) instead of this one. */
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

/* ── Double-buffered bulk read ──────────────────────────────────────────
 *
 * The previous single-buffer design caused RTL-SDR FIFO overflow and
 * sample drops: on each call we (1) submit a transfer, (2) wait for it
 * to complete, (3) memcpy the data, (4) return. Between (4) and the
 * next (1), the RTL's USB endpoint is idle. The RTL is producing data
 * continuously at ~2 MB/s and its internal FIFO fills during that
 * idle gap. If the gap is >~4ms the FIFO overflows and samples are
 * silently dropped — audible in ADS-B as dropped messages and extra
 * CRC failures.
 *
 * Separately, idle bulk gaps correlate with EP 0 STALL events from the
 * ESP-IDF hub driver. Those STALLs emit `esp_rom_printf` from ISR
 * context directly to the UART FIFO, bypassing our log hooks and
 * corrupting TUI rendering. Fixing the bulk path fixes the STALL
 * spam as a side effect — far more important than the sample drops.
 *
 * Fix: maintain TWO transfers. While the caller uses buffer A, buffer
 * B is already submitted and filling. When the caller returns, they
 * swap: await B, submit a new A. The USB endpoint never idles.
 *
 * State: s_xfer[0..1]       — the two transfers (lazily allocated)
 *        s_xfer_sem[0..1]   — per-transfer completion semaphores
 *        s_current          — which transfer the caller is "on" right now
 *        s_inflight         — does the OTHER transfer currently have one
 *                             submitted? (false on first call, true
 *                             afterwards, reset to false on any error)
 */
#define BULK_XFER_SLOTS 2
static usb_transfer_t   *s_xfer[BULK_XFER_SLOTS]     = {NULL, NULL};
static SemaphoreHandle_t s_xfer_sem[BULK_XFER_SLOTS] = {NULL, NULL};
static bool              s_xfer_ok[BULK_XFER_SLOTS]  = {false, false};
static int               s_xfer_bytes[BULK_XFER_SLOTS] = {0, 0};
static int               s_current   = 0;
static bool              s_inflight  = false;
static size_t            s_xfer_size = 0;

static void bulk_transfer_read_cb_pp(usb_transfer_t *transfer)
{
    int slot = (int)(intptr_t)transfer->context;
    if (slot < 0 || slot >= BULK_XFER_SLOTS) return;
    s_xfer_ok[slot]    = (transfer->status == 0);
    s_xfer_bytes[slot] = transfer->actual_num_bytes;
    xSemaphoreGive(s_xfer_sem[slot]);
}

/* One-time setup of the two transfer slots. Runs on first call. */
static int bulk_xfer_init(class_driver_t *driver_obj, int length,
                          unsigned char endpoint)
{
    s_xfer_size = usb_round_up_to_mps(length, 512);
    ESP_LOGI(TAG_ADSB,
             "bulk_xfer_init: length=%d size=%u ep=0x%02x dev_hdl=%p driver=%p",
             length, (unsigned)s_xfer_size, endpoint,
             driver_obj ? driver_obj->dev_hdl : NULL,
             (void *)driver_obj);
    for (int i = 0; i < BULK_XFER_SLOTS; i++) {
        s_xfer_sem[i] = xSemaphoreCreateBinary();
        if (!s_xfer_sem[i]) {
            ESP_LOGE(TAG_ADSB, "bulk_xfer_init: sem alloc slot %d failed", i);
            return -1;
        }
        esp_err_t r = usb_host_transfer_alloc(s_xfer_size, 0, &s_xfer[i]);
        if (r != ESP_OK) {
            ESP_LOGE(TAG_ADSB,
                     "bulk_xfer_init: xfer alloc slot %d failed: 0x%x size=%u",
                     i, r, (unsigned)s_xfer_size);
            return -1;
        }
        s_xfer[i]->num_bytes        = s_xfer_size;
        s_xfer[i]->device_handle    = driver_obj->dev_hdl;
        s_xfer[i]->bEndpointAddress = endpoint;
        s_xfer[i]->callback         = bulk_transfer_read_cb_pp;
        s_xfer[i]->context          = (void *)(intptr_t)i;
    }
    return 0;
}

int esp_libusb_bulk_transfer(class_driver_t *driver_obj, unsigned char endpoint,
                             unsigned char *data, int length, int *transferred,
                             unsigned int timeout)
{
    /* Lazy init on first call. */
    if (!s_xfer[0]) {
        if (bulk_xfer_init(driver_obj, length, endpoint) != 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            return -1;
        }
    }

    /* If nothing is in flight (first call ever, or after an error),
     * submit the CURRENT slot and wait for it. Same behaviour the old
     * sync transfer had, but only happens once per stream (or after
     * an error). Rate-limit the error log at 1 Hz to avoid scrollback
     * flooding on persistent failures. */
    if (!s_inflight) {
        s_xfer[s_current]->timeout_ms = timeout;
        xSemaphoreTake(s_xfer_sem[s_current], 0); /* clear stale signal */

        esp_err_t r = ESP_FAIL;
        for (int attempt = 0; attempt < 3; attempt++) {
            r = usb_host_transfer_submit(s_xfer[s_current]);
            if (r == ESP_OK) break;
            /* 0x10C == ESP_ERR_NOT_FINISHED — brief wait and retry. */
            if (r == 0x10C) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            break;
        }
        if (r != ESP_OK) {
            static int64_t s_last_err_log = 0;
            extern int64_t esp_timer_get_time(void);
            int64_t now = esp_timer_get_time();
            if (now - s_last_err_log > 1000000LL) {
                ESP_LOGE(TAG_ADSB,
                         "bulk submit (cold) failed: %d", r);
                s_last_err_log = now;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            return -1;
        }
    }

    /* Wait for the current slot to complete. If we're warm, it may
     * already be done by the time we get here (caller took its time
     * processing the previous buffer while this one was filling). */
    if (xSemaphoreTake(s_xfer_sem[s_current],
                       pdMS_TO_TICKS(timeout + 500)) != pdTRUE) {
        ESP_LOGE(TAG_ADSB, "Bulk transfer timed out (slot %d)", s_current);
        s_inflight = false;
        return -1;
    }

    if (!s_xfer_ok[s_current]) {
        ESP_LOGW(TAG_ADSB, "bulk STALL/Fail (slot %d)", s_current);
        usb_host_endpoint_clear(driver_obj->dev_hdl, endpoint);
        s_inflight = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        return -1;
    }

    /* Save bytes+pointer for return AFTER we submit the next transfer,
     * so the USB endpoint re-arms as fast as possible. */
    int      done_bytes = s_xfer_bytes[s_current];
    uint8_t *done_buf   = s_xfer[s_current]->data_buffer;

    /* Flip to the OTHER slot and submit it immediately. This is the
     * key step: the RTL-SDR's FIFO will now be actively drained by
     * the time the caller is done with our memcpy below. */
    int next = s_current ^ 1;
    s_xfer[next]->timeout_ms = timeout;
    xSemaphoreTake(s_xfer_sem[next], 0); /* clear stale */

    esp_err_t r = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        r = usb_host_transfer_submit(s_xfer[next]);
        if (r == ESP_OK) break;
        if (r == 0x10C) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        break;
    }
    if (r != ESP_OK) {
        static int64_t s_last_resub_log = 0;
        extern int64_t esp_timer_get_time(void);
        int64_t now = esp_timer_get_time();
        if (now - s_last_resub_log > 1000000LL) {
            ESP_LOGE(TAG_ADSB, "bulk resubmit failed: %d", r);
            s_last_resub_log = now;
        }
        s_inflight = false;
        /* Still copy out the completed buffer since we have it — caller
         * gets good data this call, cold-start next call. */
        *transferred = done_bytes;
        memcpy(data, done_buf, done_bytes);
        return 0;
    }
    s_inflight = true;

    /* Now copy data to caller. The OTHER transfer is already flying,
     * so the RTL FIFO is being drained while we memcpy. */
    *transferred = done_bytes;
    memcpy(data, done_buf, done_bytes);

    /* Advance for next call: caller's "current" becomes the one we
     * just submitted. */
    s_current = next;

    return 0;
}

/* ── Control transfer path (EP0) ────────────────────────────────────── */

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

    /* Clear any stale signal from a previous call that timed out. */
    xSemaphoreTake(adsbdev->done_sem, 0);

    esp_err_t r = usb_host_transfer_submit_control(driver_obj->client_hdl, adsbdev->transfer);
    if (r != ESP_OK) {
        ESP_LOGE(TAG_ADSB, "libusb_control_transfer failed to submit: %d", r);
        vTaskDelay(pdMS_TO_TICKS(50)); /* let hub task recover EP0 */
        return -1;
    }

    if (xSemaphoreTake(adsbdev->done_sem, pdMS_TO_TICKS(timeout + 500)) != pdTRUE) {
        ESP_LOGE(TAG_ADSB, "Control transfer timed out");
        return -1;
    }

    if (!adsbdev->is_success) {
        ESP_LOGW(TAG_ADSB, "libusb_control_transfer STALL/Fail");
        vTaskDelay(pdMS_TO_TICKS(50)); /* let hub task clear EP0 STALL */
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
