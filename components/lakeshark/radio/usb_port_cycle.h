/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef USB_PORT_CYCLE_H
#define USB_PORT_CYCLE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The sequence of a root-port power cycle, apart from the USB stack
   it drives, so the ordering rules below are held by the bench rather than
   by whoever next edits the firmware glue. usb_host.c binds these to
   usb_host_lib_set_root_port_power, usb_host_lib_info and vTaskDelay. */
typedef struct {
    esp_err_t (*set_power)(bool on);
    /* Devices the host library currently holds; negative if it cannot say. */
    int (*device_count)(void);
    void (*delay_ms)(uint32_t ms);
} usb_port_cycle_ops_t;

typedef enum {
    USB_PORT_CYCLE_DONE = 0,     /* off, device released, back on */
    USB_PORT_CYCLE_NOTHING,      /* nothing enumerated; port not touched */
    USB_PORT_CYCLE_OFF_FAILED,   /* power-off refused; port not touched */
    USB_PORT_CYCLE_NOT_RELEASED, /* device outlived the power-off; port on */
    USB_PORT_CYCLE_STILL_OFF,    /* power-on never accepted; USB is down */
} usb_port_cycle_result_t;

#define USB_PORT_CYCLE_POLL_MS        20u
#define USB_PORT_CYCLE_RELEASE_MS   1500u
#define USB_PORT_CYCLE_HOLD_OFF_MS   100u
#define USB_PORT_CYCLE_POWER_ON_MS  2000u

usb_port_cycle_result_t usb_port_cycle_run(const usb_port_cycle_ops_t *ops);
const char *usb_port_cycle_result_name(usb_port_cycle_result_t result);

#ifdef __cplusplus
}
#endif

#endif
