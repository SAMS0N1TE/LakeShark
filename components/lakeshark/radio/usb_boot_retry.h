/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef USB_BOOT_RETRY_H
#define USB_BOOT_RETRY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the root port itself says, apart from what enumerated. A device that
   is plugged in and failed to enumerate is PRESENT here, which is the case
   boot recovery exists for; an empty port is NONE, which it does not. */
typedef enum {
    USB_PORT_ATTACH_UNKNOWN = -1,  /* cannot tell: act as if attached */
    USB_PORT_ATTACH_NONE    = 0,
    USB_PORT_ATTACH_PRESENT = 1,
} usb_port_attach_t;

typedef struct {
    int64_t next_us;
    unsigned attempts;
    bool off, done;
    bool skipped;   /* ended because the port was empty, never cycled */
} usb_boot_retry_t;

/* One step of boot recovery. devices is what reached the USB client (negative
   when it cannot be read), attached is a usb_port_attach_t, set_power drives
   the root port. Never powers the port off while attached is NONE. */
void usb_boot_retry_tick(usb_boot_retry_t *retry, int64_t now, int devices,
                         int attached, esp_err_t (*set_power)(bool on));

/* Whether "SDR not enumerated, replug" (or the cycle that stands in for it)
   is called for: only when something is attached, or the port cannot say. A
   port that reads empty has nothing to replug. */
bool usb_absent_notice_wanted(int attached);

#ifdef __cplusplus
}
#endif

#endif
