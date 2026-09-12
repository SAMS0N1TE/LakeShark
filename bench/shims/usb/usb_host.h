#ifndef LS_SHIM_USB_HOST_H
#define LS_SHIM_USB_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *usb_host_client_handle_t;
typedef void *usb_device_handle_t;

typedef struct usb_transfer usb_transfer_t;
typedef void (*usb_transfer_cb_t)(usb_transfer_t *transfer);

struct usb_transfer {
    int status;
    int actual_num_bytes;
    size_t num_bytes;
    unsigned timeout_ms;
    usb_device_handle_t device_handle;
    uint8_t bEndpointAddress;
    usb_transfer_cb_t callback;
    void *context;
    uint8_t *data_buffer;
};

typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_packet_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wData[];
} usb_str_desc_t;

#define USB_BM_REQUEST_TYPE_TYPE_VENDOR 0x40
#define USB_BM_REQUEST_TYPE_DIR_OUT     0x00
#define USB_BM_REQUEST_TYPE_DIR_IN      0x80

static inline size_t usb_round_up_to_mps(size_t size, size_t mps)
{
    return mps ? ((size + mps - 1) / mps) * mps : size;
}

esp_err_t usb_host_transfer_alloc(size_t data_buffer_size, int flags,
                                  usb_transfer_t **transfer);
esp_err_t usb_host_transfer_free(usb_transfer_t *transfer);
esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer);
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t client,
                                           usb_transfer_t *transfer);
esp_err_t usb_host_endpoint_halt(usb_device_handle_t device,
                                 uint8_t endpoint);
esp_err_t usb_host_endpoint_flush(usb_device_handle_t device,
                                  uint8_t endpoint);
esp_err_t usb_host_endpoint_clear(usb_device_handle_t device,
                                  uint8_t endpoint);

#ifdef __cplusplus
}
#endif

#endif
