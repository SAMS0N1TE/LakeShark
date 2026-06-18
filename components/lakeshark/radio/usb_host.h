
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
