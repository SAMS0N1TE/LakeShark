/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_ESP_LIBUSB_PRIVATE_H
#define LS_ESP_LIBUSB_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"

#ifndef portMAX_DELAY
#define portMAX_DELAY (TickType_t)0xffffffffUL
#endif

#define CTRL_OUT (USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_DIR_OUT)
#define CTRL_IN (USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_DIR_IN)

#define USB_SETUP_PACKET_INIT_CONTROL(setup_pkt_ptr, bm_reqtype, b_request, w_value, w_index, w_length) ({ \
    (setup_pkt_ptr)->bmRequestType = bm_reqtype;                                                           \
    (setup_pkt_ptr)->bRequest = b_request;                                                                 \
    (setup_pkt_ptr)->wValue = w_value;                                                                     \
    (setup_pkt_ptr)->wIndex = w_index;                                                                     \
    (setup_pkt_ptr)->wLength = w_length;                                                                   \
})

typedef struct {
    usb_host_client_handle_t client_hdl;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    uint32_t actions;
} class_driver_t;

typedef struct {
    bool is_adsb;
    uint8_t *response_buf;
    bool is_success;
    int bytes_transferred;
    usb_transfer_t *transfer;
    SemaphoreHandle_t done_sem;
} class_adsb_dev;

#define TAG_ADSB "ADSB"
#define ESP_LIBUSB_ERR_BUSY (-2)
#define ESP_LIBUSB_ERR_NO_MEM (-3)

void init_adsb_dev(void);
void bulk_transfer_read_cb(usb_transfer_t *transfer);
void transfer_read_cb(usb_transfer_t *transfer);
int esp_libusb_bulk_transfer(class_driver_t *driver_obj,
                             unsigned char endpoint, unsigned char *data,
                             int length, int *transferred,
                             unsigned int timeout);
void esp_libusb_bulk_teardown(void);
void esp_libusb_bulk_teardown_for(class_driver_t *driver_obj);

int esp_libusb_stream_start(class_driver_t *driver_obj,
                            unsigned char endpoint);
void esp_libusb_stream_stop(void);
void esp_libusb_stream_stop_for(class_driver_t *driver_obj);
bool esp_libusb_stream_owned_by(const class_driver_t *driver_obj);
int esp_libusb_stream_read(unsigned char *dst, int max);
int esp_libusb_stream_read_timeout(unsigned char *dst, int max,
                                   uint32_t timeout_ms);
void esp_libusb_stream_reset(void);
uint32_t esp_libusb_stream_avail(void);
uint64_t esp_libusb_stream_dropped(void);
int esp_libusb_stream_slots(void);
uint32_t esp_libusb_total_bytes(void);
bool esp_libusb_streaming(void);
bool esp_libusb_bulk_active(void);
void esp_libusb_note_device_gone(usb_device_handle_t device);
int esp_libusb_control_transfer(class_driver_t *driver_obj,
                                uint8_t bm_req_type, uint8_t b_request,
                                uint16_t wValue, uint16_t wIndex,
                                unsigned char *data, uint16_t wLength,
                                unsigned int timeout);
void esp_libusb_get_string_descriptor_ascii(const usb_str_desc_t *str_desc,
                                            char *str);

#endif
