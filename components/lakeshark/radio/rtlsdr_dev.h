
#ifndef RTLSDR_DEV_H
#define RTLSDR_DEV_H

#include <stdint.h>
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

void rtlsdr_dev_setup_async(uint8_t dev_addr, usb_host_client_handle_t client);
int  rtlsdr_dev_reopen(void);

uint32_t rtlsdr_dev_set_freq(uint32_t hz);
int      rtlsdr_dev_set_gain(int tenths_db);
int      rtlsdr_dev_set_sample_rate(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif
