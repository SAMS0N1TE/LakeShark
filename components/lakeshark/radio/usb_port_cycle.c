/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "usb_port_cycle.h"

#include <stddef.h>

/* A root-port power cycle is the nearest thing to a replug that a
   board with no VBUS switch has. VBUS stays up - the dongle never loses
   power - but the host drops the port, the stack releases the device, and
   powering the port again gives the dongle a fresh bus reset and a fresh
   enumeration from address 0. That bus reset is the part of a reboot that
   revived the wedged R820T on the T-Display-P4 on 2026-09-11 with no replug.

   Two IDF 5.4.3 rules fix the order, and breaking either ends in abort():

   - Never power off with nothing enumerated. On a disconnect with the port
     unpowered, hub.c assumes a device node exists and ESP_ERROR_CHECKs
     dev_tree_node_dev_gone(), which returns NOT_FOUND when there is none -
     for example while the hub is still looping on "Root port reset failed"
     for a dongle that has not come up yet.

   - Do not power back on until the device is released. Power-on is refused
     with INVALID_STATE, not an abort, until the hub has recycled the port,
     so it is retried rather than taken as final.

   One race stays open from outside the stack: a real unplug in the few
   milliseconds between the count below and the power-off can still reach
   hub_root_stop's assert or root_port_recycle's abort. The firmware narrows
   it by running this below the USB lib task's priority, on that task's core,
   so a port event is always handled before this can act on the port. */
usb_port_cycle_result_t usb_port_cycle_run(const usb_port_cycle_ops_t *ops)
{
    if (!ops || !ops->set_power || !ops->device_count || !ops->delay_ms)
        return USB_PORT_CYCLE_NOTHING;
    if (ops->device_count() < 1) return USB_PORT_CYCLE_NOTHING;
    if (ops->set_power(false) != ESP_OK) return USB_PORT_CYCLE_OFF_FAILED;

    bool released = false;
    for (uint32_t waited = 0; ; waited += USB_PORT_CYCLE_POLL_MS) {
        if (ops->device_count() == 0) {
            released = true;
            break;
        }
        if (waited >= USB_PORT_CYCLE_RELEASE_MS) break;
        ops->delay_ms(USB_PORT_CYCLE_POLL_MS);
    }
    /* Powered back on whether or not the device let go: a port left off is
       USB gone until a reboot, which costs more than a stale device object. */
    ops->delay_ms(USB_PORT_CYCLE_HOLD_OFF_MS);

    esp_err_t on = ESP_FAIL;
    for (uint32_t waited = 0; ; waited += USB_PORT_CYCLE_POLL_MS) {
        on = ops->set_power(true);
        if (on == ESP_OK || waited >= USB_PORT_CYCLE_POWER_ON_MS) break;
        ops->delay_ms(USB_PORT_CYCLE_POLL_MS);
    }
    if (on != ESP_OK) return USB_PORT_CYCLE_STILL_OFF;
    return released ? USB_PORT_CYCLE_DONE : USB_PORT_CYCLE_NOT_RELEASED;
}

const char *usb_port_cycle_result_name(usb_port_cycle_result_t result)
{
    switch (result) {
    case USB_PORT_CYCLE_DONE:         return "done";
    case USB_PORT_CYCLE_NOTHING:      return "nothing enumerated";
    case USB_PORT_CYCLE_OFF_FAILED:   return "power-off refused";
    case USB_PORT_CYCLE_NOT_RELEASED: return "device not released";
    case USB_PORT_CYCLE_STILL_OFF:    return "port did not power back on";
    }
    return "?";
}
