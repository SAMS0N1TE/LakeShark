/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */

#ifndef RTLSDR_DEV_H
#define RTLSDR_DEV_H

#include <stdbool.h>
#include <stdint.h>

#include "usb_port_cycle.h"

#ifdef __cplusplus
extern "C" {
#endif

/**/
void rtlsdr_dev_teardown(void);

/* Release the dongle in the detach-safe teardown order, then power cycle the USB
   root port: the software stand-in for a replug on a board with no VBUS
   switch. Blocks for up to a few seconds; call it from a task of its own. */
usb_port_cycle_result_t rtl_adapter_port_reset(void);
/* Whether the host holds an enumerated device for that reset to act on. */
bool rtl_adapter_port_reset_possible(void);

#ifdef __cplusplus
}
#endif

#endif
