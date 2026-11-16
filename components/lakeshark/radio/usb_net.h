/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. USB Ethernet adapters (CDC-ECM, RNDIS) on the USB host
   port, as a second device type beside the SDR dongles. One adapter at a
   time. CONFIG_LS_USB_ETH; the network side (esp_netif, DHCP, the 'eth'
   command) is main/ls_eth.c. */
#ifndef LS_USB_NET_H
#define LS_USB_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* From the class driver's probe, after the descriptor probe has closed its
   handle. search_configs: the active configuration has no network function;
   look in the others, and hand the device to the RTL-SDR path if none has. */
void ls_usb_net_probe_async(uint8_t dev_addr, usb_host_client_handle_t client,
                            bool search_configs);
/* USB_HOST_CLIENT_EVENT_DEV_GONE. True if the device was the adapter. */
bool ls_usb_net_note_removed(usb_device_handle_t device);
/* Enumeration filter (USB lib task): picks a learned configuration. */
bool ls_usb_net_enum_filter(const usb_device_desc_t *dev_desc, uint8_t *config_value);

/* Network side. Every callback is short and must not block: rx and link run
   in the USB client task, attached in the adapter's setup task. */
typedef struct {
    void (*attached)(const uint8_t mac[6]);
    void (*detached)(void);
    void (*link)(bool up, uint32_t bps);
    void (*rx)(const uint8_t *frame, size_t len);
} ls_usb_net_sink_t;
void ls_usb_net_set_sink(const ls_usb_net_sink_t *sink);

/* Queues one Ethernet frame (14..1514 bytes). ESP_ERR_NO_MEM when every TX
   transfer is in flight (the stack retries), ESP_ERR_INVALID_STATE with no
   adapter. Any task. */
esp_err_t ls_usb_net_tx(const void *frame, size_t len);

typedef struct {
    bool present, link;
    uint8_t mac[6];
    uint32_t bps;            /* 0 when the adapter has not said */
    const char *kind;        /* "ECM" / "RNDIS" */
    uint16_t vid, pid;
    uint32_t rx_errors, tx_drops;
} ls_usb_net_status_t;
void ls_usb_net_status(ls_usb_net_status_t *out);

#ifdef __cplusplus
}
#endif

#endif
