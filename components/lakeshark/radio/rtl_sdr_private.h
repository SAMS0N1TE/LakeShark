/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_RTL_SDR_PRIVATE_H
#define LS_RTL_SDR_PRIVATE_H

#include <stdint.h>

#include "rtl-sdr.h"
#include "usb/usb_host.h"

int rtlsdr_open(rtlsdr_dev_t **dev, uint8_t index,
                usb_host_client_handle_t client_hdl);
void esp_action_get_dev_desc(rtlsdr_dev_t *dev);
int rtlsdr_stream_read_timeout(void *buf, int max, uint32_t timeout_ms);
void rtlsdr_stream_stop_for(rtlsdr_dev_t *dev);
usb_device_handle_t rtlsdr_usb_device_handle(rtlsdr_dev_t *dev);
/* librtlsdr's raw register access (vendor request 0, wValue the address,
   wIndex the block << 8, | 0x10 to write): answers the bytes moved. */
int rtlsdr_read_array(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr,
                      uint8_t *array, uint8_t len);
int rtlsdr_write_reg(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr,
                     uint16_t val, uint8_t len);

#endif
