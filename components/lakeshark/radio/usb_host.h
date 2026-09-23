/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */

#ifndef USB_HOST_H
#define USB_HOST_H

#include <stdbool.h>

#include "usb_port_cycle.h"

#ifdef __cplusplus
extern "C" {
#endif

void class_driver_task(void *arg);
void class_driver_client_deregister(void);

/* Root-port power cycle through the IDF host library. See
   usb_port_cycle.c for the order it keeps and the race it cannot close. The
   caller must already have closed every handle it holds on the device. */
bool usb_host_root_port_has_device(void);
usb_port_cycle_result_t usb_host_root_port_cycle(void);

#ifdef __cplusplus
}
#endif

#endif
