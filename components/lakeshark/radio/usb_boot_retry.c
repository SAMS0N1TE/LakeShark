/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "usb_boot_retry.h"

#include "esp_log.h"

static const char *TAG = "usb_host";

void usb_boot_retry_tick(usb_boot_retry_t *retry, int64_t now, int devices,
                         int attached, esp_err_t (*set_power)(bool on))
{
    if(retry->done) return;
    /* A successful attach permanently ends boot recovery. Never disturb a
       working receiver, hub or any other registered USB peripheral.

       SAY SO WHEN IT HAPPENS. A ten-reset series recovered nine times;
       the tenth failed CHECK_SHORT_DEV_DESC at 4541 ms - the exact
       failure this incident records - and then no retry ever ran. The
       suspect is this line: a device that FAILED enumeration may still
       be counted by usb_host_lib_info, in which case devices > 0 is
       true, recovery concludes something attached and disables itself
       permanently, in precisely the case it exists for.

       It was: after "Root port reset failed" and CHECK_SHORT_DEV_DESC
       FAILED the library still reported one device and recovery stood
       down with no dongle attached. The caller now passes the devices that
       actually reached this client, which a failed enumeration never does. */
    if(devices > 0 && !retry->off) {
        ESP_LOGW(TAG,"USB boot recovery stood down: %d device(s) "
                     "enumerated at %lld ms",devices,
                     (long long)(now/1000));
        retry->done=true;
        return;
    }
    if(devices < 0 || now < retry->next_us) return;
    if(retry->off) {
        esp_err_t err=set_power(true);
        retry->off=false;
        retry->next_us=now+5000000;
        if(err != ESP_OK) retry->done=true;
        ESP_LOGW(TAG,"USB boot retry %u: host port on: %s",retry->attempts,esp_err_to_name(err));
        return;
    }
    if(retry->attempts>=3) { retry->done=true; return; }
    /* Nothing plugged in is nothing to recover. The port is only cycled for
       a device that is attached and has not enumerated (USB-BOOT-ENUM); an
       empty port cycled three times is 16 s of toggling for nobody. Only a
       port that positively reads empty ends it: unknown keeps the cycle.
       The host stack stays installed, so a later plug still enumerates. */
    if(attached == USB_PORT_ATTACH_NONE) {
        retry->done=true;
        retry->skipped=true;
        ESP_LOGW(TAG,"USB boot recovery skipped: nothing attached to the host port");
        return;
    }
    retry->attempts++;
    esp_err_t err=set_power(false);
    retry->off=err==ESP_OK;
    retry->next_us=now+(retry->off?300000:5000000);
    ESP_LOGW(TAG,"USB boot retry %u: no enumerated devices, host port off: %s",retry->attempts,esp_err_to_name(err));
}

bool usb_absent_notice_wanted(int attached)
{
    return attached != USB_PORT_ATTACH_NONE;
}
