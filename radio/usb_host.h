/*
 * usb_host.h - USB host class driver entry points.
 *
 * The class driver task runs on CPU0, owns the USB host client handle,
 * and dispatches device-attach events to rtlsdr_dev_setup_async().
 * App code never calls this directly except from app_main(); it's just
 * factored out of class_driver.c so the dependency graph is clean.
 */
#ifndef USB_HOST_H
#define USB_HOST_H

#include <stdint.h>
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

void class_driver_task(void *arg);
void class_driver_client_deregister(void);

usb_host_client_handle_t class_driver_client_handle(void);

#ifdef __cplusplus
}
#endif

#endif
