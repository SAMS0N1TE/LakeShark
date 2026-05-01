/*
 * rtlsdr_dev.h - RTL-SDR device setup and IQ streaming.
 *
 * On USB attach, usb_host.c calls rtlsdr_dev_setup_async() which spawns
 * a task that performs control transfers to open the dongle, set up the
 * tuner, and finally spawn the per-app sample loop. The rtlsdr_dev
 * module itself is app-agnostic; the sample loop calls back into the
 * active app via the app_t::on_sample callback.
 */
#ifndef RTLSDR_DEV_H
#define RTLSDR_DEV_H

#include <stdint.h>
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

void rtlsdr_dev_setup_async(uint8_t dev_addr, usb_host_client_handle_t client);

uint32_t rtlsdr_dev_set_freq(uint32_t hz);
int      rtlsdr_dev_set_gain(int tenths_db);
int      rtlsdr_dev_set_sample_rate(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif
