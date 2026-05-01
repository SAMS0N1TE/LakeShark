/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012-2013 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __RTL_SDR_H
#define __RTL_SDR_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "usb/usb_host.h"
#include <rtl-sdr_export.h>

    typedef struct rtlsdr_dev rtlsdr_dev_t;

    RTLSDR_API void esp_action_get_dev_desc(rtlsdr_dev_t *dev);
    RTLSDR_API int rtlsdr_open(rtlsdr_dev_t **dev, uint8_t index, usb_host_client_handle_t client_hdl);

    RTLSDR_API int rtlsdr_close(rtlsdr_dev_t *dev);

    RTLSDR_API int rtlsdr_set_xtal_freq(rtlsdr_dev_t *dev, uint32_t rtl_freq,
                                        uint32_t tuner_freq);

    RTLSDR_API int rtlsdr_get_xtal_freq(rtlsdr_dev_t *dev, uint32_t *rtl_freq,
                                        uint32_t *tuner_freq);

    RTLSDR_API int rtlsdr_get_usb_strings(rtlsdr_dev_t *dev, char *manufact,
                                          char *product, char *serial);

    RTLSDR_API int rtlsdr_write_eeprom(rtlsdr_dev_t *dev, uint8_t *data,
                                       uint8_t offset, uint16_t len);

    RTLSDR_API int rtlsdr_read_eeprom(rtlsdr_dev_t *dev, uint8_t *data,
                                      uint8_t offset, uint16_t len);

    RTLSDR_API int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t freq);

    RTLSDR_API uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev);

    RTLSDR_API int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm);

    RTLSDR_API int rtlsdr_get_freq_correction(rtlsdr_dev_t *dev);

    enum rtlsdr_tuner
    {
        RTLSDR_TUNER_UNKNOWN = 0,
        RTLSDR_TUNER_E4000,
        RTLSDR_TUNER_FC0012,
        RTLSDR_TUNER_FC0013,
        RTLSDR_TUNER_FC2580,
        RTLSDR_TUNER_R820T,
        RTLSDR_TUNER_R828D
    };

    RTLSDR_API enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t *dev);

    RTLSDR_API int rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains);

    RTLSDR_API int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain);

    RTLSDR_API int rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t *dev, uint32_t bw);

    RTLSDR_API int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev);

    RTLSDR_API int rtlsdr_set_tuner_if_gain(rtlsdr_dev_t *dev, int stage, int gain);

    RTLSDR_API int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int manual);

    RTLSDR_API int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t rate);

    RTLSDR_API uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev);

    RTLSDR_API int rtlsdr_set_testmode(rtlsdr_dev_t *dev, int on);

    RTLSDR_API int rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on);

    RTLSDR_API int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on);

    RTLSDR_API int rtlsdr_get_direct_sampling(rtlsdr_dev_t *dev);

    RTLSDR_API int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on);

    RTLSDR_API int rtlsdr_get_offset_tuning(rtlsdr_dev_t *dev);

    /* streaming functions */

    RTLSDR_API int rtlsdr_reset_buffer(rtlsdr_dev_t *dev);

    RTLSDR_API int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read);

    typedef void (*rtlsdr_read_async_cb_t)(unsigned char *buf, uint32_t len, void *ctx);

    RTLSDR_API int rtlsdr_wait_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx);

    RTLSDR_API int rtlsdr_read_async(rtlsdr_dev_t *dev,
                                     rtlsdr_read_async_cb_t cb,
                                     void *ctx,
                                     uint32_t buf_num,
                                     uint32_t buf_len);

    RTLSDR_API int rtlsdr_cancel_async(rtlsdr_dev_t *dev);

    RTLSDR_API int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on);

    RTLSDR_API int rtlsdr_set_bias_tee_gpio(rtlsdr_dev_t *dev, int gpio, int on);

#ifdef __cplusplus
}
#endif

#endif /* __RTL_SDR_H */
