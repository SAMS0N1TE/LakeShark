/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_HACKRF_ADAPTER_PRIVATE_H
#define LS_HACKRF_ADAPTER_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

#include "usb/usb_host.h"

void hackrf_adapter_probe_async(uint8_t dev_addr,
                                usb_host_client_handle_t client);
bool hackrf_adapter_note_removed(usb_device_handle_t device);

#endif
