/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012-2014 by Steve Markgraf <steve@steve-m.de>
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

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#ifndef _WIN32
#include <unistd.h>
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#include <esp_libusb.h>

/* librtlsdr has hundreds of fprintf(stderr, ...) calls scattered through
 * its codebase. On desktop Linux these show up in the terminal; on our
 * ESP32 they bypass ESP_LOG (which the TUI hooks) AND they bypass the
 * UART driver's TX buffer — going straight through stderr which lands
 * on a different VFS path. Result: raw text leaks past the TUI border
 * and shreds the screen.
 *
 * Redefine fprintf(stderr, ...) to funnel through ESP_LOGW so the TUI
 * log hook captures it. Uses a comma operator trick because fprintf
 * has a different signature than ESP_LOGW — we discard the stream
 * argument and just format the rest. */
#undef fprintf
#define fprintf(stream, ...)  ESP_LOGW("RTLSDR", __VA_ARGS__)

/* two raised to the power of n */
#define TWO_POW(n) ((double)(1ULL << (n)))

#include "rtl-sdr.h"
#include "tuner_e4k.h"
#include "tuner_fc0012.h"
#include "tuner_fc0013.h"
#include "tuner_fc2580.h"
#include "tuner_r82xx.h"

typedef struct rtlsdr_tuner_iface
{
    /* tuner interface */
    int (*init)(void *);
    int (*exit)(void *);
    int (*set_freq)(void *, uint32_t freq /* Hz */);
    int (*set_bw)(void *, int bw /* Hz */);
    int (*set_gain)(void *, int gain /* tenth dB */);
    int (*set_if_gain)(void *, int stage, int gain /* tenth dB */);
    int (*set_gain_mode)(void *, int manual);
} rtlsdr_tuner_iface_t;

enum rtlsdr_async_status
{
    RTLSDR_INACTIVE = 0,
    RTLSDR_CANCELING,
    RTLSDR_RUNNING
};

#define FIR_LEN 16

/*
 * FIR coefficients.
 *
 * The filter is running at XTal frequency. It is symmetric filter with 32
 * coefficients. Only first 16 coefficients are specified, the other 16
 * use the same values but in reversed order. The first coefficient in
 * the array is the outer one, the last, the last is the inner one.
 * First 8 coefficients are 8 bit signed integers, the next 8 coefficients
 * are 12 bit signed integers. All coefficients have the same weight.
 *
 * Default FIR coefficients used for DAB/FM by the Windows driver,
 * the DVB driver uses different ones
 */
static const int fir_default[FIR_LEN] = {
    -54, -36, -41, -40, -32, -14, 14, 53,  /* 8 bit signed */
    101, 156, 215, 273, 327, 372, 404, 421 /* 12 bit signed */
};

struct rtlsdr_dev
{
    class_driver_t *driver_obj;
    uint32_t xfer_buf_num;
    uint32_t xfer_buf_len;
    struct libusb_transfer **xfer;
    unsigned char **xfer_buf;
    rtlsdr_read_async_cb_t cb;
    void *cb_ctx;
    enum rtlsdr_async_status async_status;
    int async_cancel;
    int use_zerocopy;
    /* rtl demod context */
    uint32_t rate;     /* Hz */
    uint32_t rtl_xtal; /* Hz */
    int fir[FIR_LEN];
    int direct_sampling;
    /* tuner context */
    enum rtlsdr_tuner tuner_type;
    rtlsdr_tuner_iface_t *tuner;
    uint32_t tun_xtal; /* Hz */
    uint32_t freq;     /* Hz */
    uint32_t bw;
    uint32_t offs_freq; /* Hz */
    int corr;           /* ppm */
    int gain;           /* tenth dB */
    struct e4k_state e4k_s;
    struct r82xx_config r82xx_c;
    struct r82xx_priv r82xx_p;
    /* status */
    int dev_lost;
    int driver_active;
    unsigned int xfer_errors;
    int i2c_repeater_on;
    /* EEPROM strings; populated in rtlsdr_open(), used by tuner driver
     * to detect RTL-SDR Blog V4 (triplexer requires special handling). */
    char manufact[256];
    char product[256];
};

void rtlsdr_set_gpio_bit(rtlsdr_dev_t *dev, uint8_t gpio, int val);
static int rtlsdr_set_if_freq(rtlsdr_dev_t *dev, uint32_t freq);

/* generic tuner interface functions, shall be moved to the tuner implementations */
int e4000_init(void *dev)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    devt->e4k_s.i2c_addr = E4K_I2C_ADDR;
    rtlsdr_get_xtal_freq(devt, NULL, &devt->e4k_s.vco.fosc);
    devt->e4k_s.rtl_dev = dev;
    return e4k_init(&devt->e4k_s);
}
int e4000_exit(void *dev)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    return e4k_standby(&devt->e4k_s, 1);
}
int e4000_set_freq(void *dev, uint32_t freq)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    return e4k_tune_freq(&devt->e4k_s, freq);
}

int e4000_set_bw(void *dev, int bw)
{
    int r = 0;
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;

    r |= e4k_if_filter_bw_set(&devt->e4k_s, E4K_IF_FILTER_MIX, bw);
    r |= e4k_if_filter_bw_set(&devt->e4k_s, E4K_IF_FILTER_RC, bw);
    r |= e4k_if_filter_bw_set(&devt->e4k_s, E4K_IF_FILTER_CHAN, bw);

    return r;
}

int e4000_set_gain(void *dev, int gain)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    int mixgain = (gain > 340) ? 12 : 4;
#if 0
	int enhgain = (gain - 420);
#endif
    if (e4k_set_lna_gain(&devt->e4k_s, min(300, gain - mixgain * 10)) == -EINVAL)
        return -1;
    if (e4k_mixer_gain_set(&devt->e4k_s, mixgain) == -EINVAL)
        return -1;
#if 0 /* enhanced mixer gain seems to have no effect */
	if(enhgain >= 0)
		if(e4k_set_enh_gain(&devt->e4k_s, enhgain) == -EINVAL)
			return -1;
#endif
    return 0;
}
int e4000_set_if_gain(void *dev, int stage, int gain)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    return e4k_if_gain_set(&devt->e4k_s, (uint8_t)stage, (int8_t)(gain / 10));
}
int e4000_set_gain_mode(void *dev, int manual)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    return e4k_enable_manual_gain(&devt->e4k_s, manual);
}

int _fc0012_init(void *dev) { return fc0012_init(dev); }
int fc0012_exit(void *dev) { return 0; }
int fc0012_set_freq(void *dev, uint32_t freq)
{
    /* select V-band/U-band filter */
    rtlsdr_set_gpio_bit(dev, 6, (freq > 300000000) ? 1 : 0);
    return fc0012_set_params(dev, freq, 6000000);
}
int fc0012_set_bw(void *dev, int bw) { return 0; }
int _fc0012_set_gain(void *dev, int gain) { return fc0012_set_gain(dev, gain); }
int fc0012_set_gain_mode(void *dev, int manual) { return 0; }

int _fc0013_init(void *dev) { return fc0013_init(dev); }
int fc0013_exit(void *dev) { return 0; }
int fc0013_set_freq(void *dev, uint32_t freq)
{
    return fc0013_set_params(dev, freq, 6000000);
}
int fc0013_set_bw(void *dev, int bw) { return 0; }
int _fc0013_set_gain(void *dev, int gain) { return fc0013_set_lna_gain(dev, gain); }

int fc2580_init(void *dev) { return fc2580_Initialize(dev); }
int fc2580_exit(void *dev) { return 0; }
int _fc2580_set_freq(void *dev, uint32_t freq)
{
    return fc2580_SetRfFreqHz(dev, freq);
}
int fc2580_set_bw(void *dev, int bw) { return fc2580_SetBandwidthMode(dev, 1); }
int fc2580_set_gain(void *dev, int gain) { return 0; }
int fc2580_set_gain_mode(void *dev, int manual) { return 0; }

int r820t_init(void *dev)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    devt->r82xx_p.rtl_dev = dev;

    if (devt->tuner_type == RTLSDR_TUNER_R828D)
    {
        devt->r82xx_c.i2c_addr = R828D_I2C_ADDR;
        devt->r82xx_c.rafael_chip = CHIP_R828D;
    }
    else
    {
        devt->r82xx_c.i2c_addr = R820T_I2C_ADDR;
        devt->r82xx_c.rafael_chip = CHIP_R820T;
    }

    rtlsdr_get_xtal_freq(devt, NULL, &devt->r82xx_c.xtal);

    devt->r82xx_c.max_i2c_msg_len = 8;
    devt->r82xx_c.use_predetect = 0;
    devt->r82xx_p.cfg = &devt->r82xx_c;

    return r82xx_init(&devt->r82xx_p);
}
int r820t_exit(void *dev)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    return r82xx_standby(&devt->r82xx_p);
}

int r820t_set_freq(void *dev, uint32_t freq)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    return r82xx_set_freq(&devt->r82xx_p, freq);
}

int r820t_set_bw(void *dev, int bw)
{
    int r;
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;

    r = r82xx_set_bandwidth(&devt->r82xx_p, bw, devt->rate);
    if (r < 0)
        return r;
    r = rtlsdr_set_if_freq(devt, r);
    if (r)
        return r;
    return rtlsdr_set_center_freq(devt, devt->freq);
}

int r820t_set_gain(void *dev, int gain)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    return r82xx_set_gain(&devt->r82xx_p, 1, gain);
}
int r820t_set_gain_mode(void *dev, int manual)
{
    rtlsdr_dev_t *devt = (rtlsdr_dev_t *)dev;
    return r82xx_set_gain(&devt->r82xx_p, manual, 0);
}

/* definition order must match enum rtlsdr_tuner */
static rtlsdr_tuner_iface_t tuners[] = {
    {
        NULL, NULL, NULL, NULL, NULL, NULL, NULL /* dummy for unknown tuners */
    },
    {e4000_init, e4000_exit,
     e4000_set_freq, e4000_set_bw, e4000_set_gain, e4000_set_if_gain,
     e4000_set_gain_mode},
    {_fc0012_init, fc0012_exit,
     fc0012_set_freq, fc0012_set_bw, _fc0012_set_gain, NULL,
     fc0012_set_gain_mode},
    {_fc0013_init, fc0013_exit,
     fc0013_set_freq, fc0013_set_bw, _fc0013_set_gain, NULL,
     fc0013_set_gain_mode},
    {fc2580_init, fc2580_exit,
     _fc2580_set_freq, fc2580_set_bw, fc2580_set_gain, NULL,
     fc2580_set_gain_mode},
    {r820t_init, r820t_exit,
     r820t_set_freq, r820t_set_bw, r820t_set_gain, NULL,
     r820t_set_gain_mode},
    {r820t_init, r820t_exit,
     r820t_set_freq, r820t_set_bw, r820t_set_gain, NULL,
     r820t_set_gain_mode},
};

typedef struct rtlsdr_dongle
{
    uint16_t vid;
    uint16_t pid;
    const char *name;
} rtlsdr_dongle_t;

/*
 * Please add your device here and send a patch to osmocom-sdr@lists.osmocom.org
 */
static rtlsdr_dongle_t known_devices[] = {
    {0x0bda, 0x2832, "Generic RTL2832U"},
    {0x0bda, 0x2838, "Generic RTL2832U OEM"},
    {0x0413, 0x6680, "DigitalNow Quad DVB-T PCI-E card"},
    {0x0413, 0x6f0f, "Leadtek WinFast DTV Dongle mini D"},
    {0x0458, 0x707f, "Genius TVGo DVB-T03 USB dongle (Ver. B)"},
    {0x0ccd, 0x00a9, "Terratec Cinergy T Stick Black (rev 1)"},
    {0x0ccd, 0x00b3, "Terratec NOXON DAB/DAB+ USB dongle (rev 1)"},
    {0x0ccd, 0x00b4, "Terratec Deutschlandradio DAB Stick"},
    {0x0ccd, 0x00b5, "Terratec NOXON DAB Stick - Radio Energy"},
    {0x0ccd, 0x00b7, "Terratec Media Broadcast DAB Stick"},
    {0x0ccd, 0x00b8, "Terratec BR DAB Stick"},
    {0x0ccd, 0x00b9, "Terratec WDR DAB Stick"},
    {0x0ccd, 0x00c0, "Terratec MuellerVerlag DAB Stick"},
    {0x0ccd, 0x00c6, "Terratec Fraunhofer DAB Stick"},
    {0x0ccd, 0x00d3, "Terratec Cinergy T Stick RC (Rev.3)"},
    {0x0ccd, 0x00d7, "Terratec T Stick PLUS"},
    {0x0ccd, 0x00e0, "Terratec NOXON DAB/DAB+ USB dongle (rev 2)"},
    {0x1554, 0x5020, "PixelView PV-DT235U(RN)"},
    {0x15f4, 0x0131, "Astrometa DVB-T/DVB-T2"},
    {0x15f4, 0x0133, "HanfTek DAB+FM+DVB-T"},
    {0x185b, 0x0620, "Compro Videomate U620F"},
    {0x185b, 0x0650, "Compro Videomate U650F"},
    {0x185b, 0x0680, "Compro Videomate U680F"},
    {0x1b80, 0xd393, "GIGABYTE GT-U7300"},
    {0x1b80, 0xd394, "DIKOM USB-DVBT HD"},
    {0x1b80, 0xd395, "Peak 102569AGPK"},
    {0x1b80, 0xd397, "KWorld KW-UB450-T USB DVB-T Pico TV"},
    {0x1b80, 0xd398, "Zaapa ZT-MINDVBZP"},
    {0x1b80, 0xd39d, "SVEON STV20 DVB-T USB & FM"},
    {0x1b80, 0xd3a4, "Twintech UT-40"},
    {0x1b80, 0xd3a8, "ASUS U3100MINI_PLUS_V2"},
    {0x1b80, 0xd3af, "SVEON STV27 DVB-T USB & FM"},
    {0x1b80, 0xd3b0, "SVEON STV21 DVB-T USB & FM"},
    {0x1d19, 0x1101, "Dexatek DK DVB-T Dongle (Logilink VG0002A)"},
    {0x1d19, 0x1102, "Dexatek DK DVB-T Dongle (MSI DigiVox mini II V3.0)"},
    {0x1d19, 0x1103, "Dexatek Technology Ltd. DK 5217 DVB-T Dongle"},
    {0x1d19, 0x1104, "MSI DigiVox Micro HD"},
    {0x1f4d, 0xa803, "Sweex DVB-T USB"},
    {0x1f4d, 0xb803, "GTek T803"},
    {0x1f4d, 0xc803, "Lifeview LV5TDeluxe"},
    {0x1f4d, 0xd286, "MyGica TD312"},
    {0x1f4d, 0xd803, "PROlectrix DV107669"},
};

#define DEFAULT_BUF_NUMBER 15
#define DEFAULT_BUF_LENGTH (16 * 32 * 512)

#define DEF_RTL_XTAL_FREQ 28800000
#define MIN_RTL_XTAL_FREQ (DEF_RTL_XTAL_FREQ - 1000)
#define MAX_RTL_XTAL_FREQ (DEF_RTL_XTAL_FREQ + 1000)

// #define CTRL_IN (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN)
// #define CTRL_OUT (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT)
#define CTRL_TIMEOUT 300
#define BULK_TIMEOUT 0

#define EEPROM_ADDR 0xa0

enum usb_reg
{
    USB_SYSCTL = 0x2000,
    USB_CTRL = 0x2010,
    USB_STAT = 0x2014,
    USB_EPA_CFG = 0x2144,
    USB_EPA_CTL = 0x2148,
    USB_EPA_MAXPKT = 0x2158,
    USB_EPA_MAXPKT_2 = 0x215a,
    USB_EPA_FIFO_CFG = 0x2160,
};

enum sys_reg
{
    DEMOD_CTL = 0x3000,
    GPO = 0x3001,
    GPI = 0x3002,
    GPOE = 0x3003,
    GPD = 0x3004,
    SYSINTE = 0x3005,
    SYSINTS = 0x3006,
    GP_CFG0 = 0x3007,
    GP_CFG1 = 0x3008,
    SYSINTE_1 = 0x3009,
    SYSINTS_1 = 0x300a,
    DEMOD_CTL_1 = 0x300b,
    IR_SUSPEND = 0x300c,
};

enum blocks
{
    DEMODB = 0,
    USBB = 1,
    SYSB = 2,
    TUNB = 3,
    ROMB = 4,
    IRB = 5,
    IICB = 6,
};

int rtlsdr_read_array(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t *array, uint8_t len)
{
    uint16_t index = (block << 8);
    int ret = esp_libusb_control_transfer(dev->driver_obj, CTRL_IN, 0, addr, index, array, len, CTRL_TIMEOUT);
    return ret;
}

int rtlsdr_write_array(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t *array, uint8_t len)
{
    uint16_t index = (block << 8) | 0x10;
    int ret = esp_libusb_control_transfer(dev->driver_obj, CTRL_OUT, 0, addr, index, array, len, CTRL_TIMEOUT);
    return ret;
}

// RTL-SDR Blog V4 requires bit-reversal for tuner I2C traffic
static uint8_t rtlsdr_bitrev8(uint8_t b)
{
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

int rtlsdr_i2c_write_reg(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t reg, uint8_t val)
{
    uint8_t data[2];
    data[0] = reg;
    data[1] = val;

    return rtlsdr_write_array(dev, IICB, i2c_addr, data, 2); 
}

uint8_t rtlsdr_i2c_read_reg(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t reg)
{
    uint8_t data = 0;
    
    rtlsdr_write_array(dev, IICB, i2c_addr, &reg, 1); 
    rtlsdr_read_array(dev, IICB, i2c_addr, &data, 1);          
    
    return data; 
}

int rtlsdr_i2c_write(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t *buffer, int len)
{
    uint16_t addr = i2c_addr;

    if (!dev)
        return -1;

    return rtlsdr_write_array(dev, IICB, addr, buffer, len);
}

int rtlsdr_i2c_read(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t *buffer, int len)
{
    uint16_t addr = i2c_addr;

    if (!dev)
        return -1;

    return rtlsdr_read_array(dev, IICB, addr, buffer, len);
}

uint16_t rtlsdr_read_reg(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t len)
{
    unsigned char data[2];
    uint16_t index = (block << 8);
    uint16_t reg;

    int r = esp_libusb_control_transfer(dev->driver_obj, CTRL_IN, 0, addr, index, data, len, CTRL_TIMEOUT);
    if (r < 0)
        fprintf(stderr, "%s failed\n", __FUNCTION__);

    reg = (data[1] << 8) | data[0];

    return reg;
}

int rtlsdr_write_reg(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint16_t val, uint8_t len)
{
    unsigned char data[2];

    uint16_t index = (block << 8) | 0x10;

    if (len == 1)
        data[0] = val & 0xff;
    else
        data[0] = val >> 8;

    data[1] = val & 0xff;

    int r = esp_libusb_control_transfer(dev->driver_obj, CTRL_OUT, 0, addr, index, data, len, CTRL_TIMEOUT);

    if (r < 0)
        fprintf(stderr, "%s failed\n", __FUNCTION__);

    return r;
}

uint16_t rtlsdr_demod_read_reg(rtlsdr_dev_t *dev, uint8_t page, uint16_t addr, uint8_t len)
{
    unsigned char data[2] = {0, 0};
    uint16_t index = page;
    uint16_t reg;
    addr = (addr << 8) | 0x20;

    int r = esp_libusb_control_transfer(dev->driver_obj, CTRL_IN, 0, addr, index, data, len, CTRL_TIMEOUT);
    if (r < 0)
        fprintf(stderr, "%s failed\n", __FUNCTION__);

    reg = (data[1] << 8) | data[0];

    return reg;
}

int rtlsdr_demod_write_reg(rtlsdr_dev_t *dev, uint8_t page, uint16_t addr, uint16_t val, uint8_t len)
{
    unsigned char data[2];
    uint16_t index = 0x10 | page;
    addr = (addr << 8) | 0x20;

    if (len == 1)
        data[0] = val & 0xff;
    else
        data[0] = val >> 8;

    data[1] = val & 0xff;
    int r = esp_libusb_control_transfer(dev->driver_obj, CTRL_OUT, 0, addr, index, data, len, CTRL_TIMEOUT);
    if (r < 0)
        fprintf(stderr, "%s failed\n", __FUNCTION__);

    rtlsdr_demod_read_reg(dev, 0x0a, 0x01, 1);

    return (r == len) ? 0 : -1;
}

void rtlsdr_set_gpio_bit(rtlsdr_dev_t *dev, uint8_t gpio, int val)
{
    uint16_t r;

    gpio = 1 << gpio;
    r = rtlsdr_read_reg(dev, SYSB, GPO, 1);
    r = val ? (r | gpio) : (r & ~gpio);
    rtlsdr_write_reg(dev, SYSB, GPO, r, 1);
}

void rtlsdr_set_gpio_output(rtlsdr_dev_t *dev, uint8_t gpio)
{
    int r;
    gpio = 1 << gpio;

    r = rtlsdr_read_reg(dev, SYSB, GPD, 1);
    rtlsdr_write_reg(dev, SYSB, GPD, r & ~gpio, 1);
    r = rtlsdr_read_reg(dev, SYSB, GPOE, 1);
    rtlsdr_write_reg(dev, SYSB, GPOE, r | gpio, 1);
}

void rtlsdr_set_i2c_repeater(rtlsdr_dev_t *dev, int on)
{
    if (on == dev->i2c_repeater_on)
        return;
    on = !!on; /* values +2 to force on */
    dev->i2c_repeater_on = on;
    rtlsdr_demod_write_reg(dev, 1, 0x01, on ? 0x18 : 0x10, 1);
}

int rtlsdr_set_fir(rtlsdr_dev_t *dev)
{
    uint8_t fir[20];

    int i;
    /* format: int8_t[8] */
    for (i = 0; i < 8; ++i)
    {
        const int val = dev->fir[i];
        if (val < -128 || val > 127)
        {
            return -1;
        }
        fir[i] = val;
    }
    /* format: int12_t[8] */
    for (i = 0; i < 8; i += 2)
    {
        const int val0 = dev->fir[8 + i];
        const int val1 = dev->fir[8 + i + 1];
        if (val0 < -2048 || val0 > 2047 || val1 < -2048 || val1 > 2047)
        {
            return -1;
        }
        fir[8 + i * 3 / 2] = val0 >> 4;
        fir[8 + i * 3 / 2 + 1] = (val0 << 4) | ((val1 >> 8) & 0x0f);
        fir[8 + i * 3 / 2 + 2] = val1;
    }

    for (i = 0; i < (int)sizeof(fir); i++)
    {
        if (rtlsdr_demod_write_reg(dev, 1, 0x1c + i, fir[i], 1))
            return -1;
    }

    return 0;
}

void rtlsdr_init_baseband(rtlsdr_dev_t *dev)
{
    unsigned int i;

    /* initialize USB */
    rtlsdr_write_reg(dev, USBB, USB_SYSCTL, 0x09, 1);
    rtlsdr_write_reg(dev, USBB, USB_EPA_MAXPKT, 0x0002, 2);
    rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x1002, 2);

    /* poweron demod */
    rtlsdr_write_reg(dev, SYSB, DEMOD_CTL_1, 0x22, 1);
    rtlsdr_write_reg(dev, SYSB, DEMOD_CTL, 0xe8, 1);

    /* reset demod (bit 3, soft_rst) */
    rtlsdr_demod_write_reg(dev, 1, 0x01, 0x14, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x01, 0x10, 1);

    /* disable spectrum inversion and adjacent channel rejection */
    rtlsdr_demod_write_reg(dev, 1, 0x15, 0x00, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x16, 0x0000, 2);

    /* clear both DDC shift and IF frequency registers  */
    for (i = 0; i < 6; i++)
        rtlsdr_demod_write_reg(dev, 1, 0x16 + i, 0x00, 1);

    //-----
    rtlsdr_demod_write_reg(dev, 1, 0x1c, 0xca, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x1d, 0xdc, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x1e, 0xd7, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x1f, 0xd8, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x20, 0xe0, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x21, 0xf2, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x22, 0x0e, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x23, 0x35, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x24, 0x06, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x25, 0x50, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x26, 0x9c, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x27, 0x0d, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x28, 0x71, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x29, 0x11, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x2a, 0x14, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x2b, 0x71, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x2c, 0x74, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x2d, 0x19, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x2e, 0x41, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x2f, 0xa5, 1);
    //----

    // rtlsdr_set_fir(dev);

    /* enable SDR mode, disable DAGC (bit 5) */
    rtlsdr_demod_write_reg(dev, 0, 0x19, 0x05, 1);

    /* init FSM state-holding register */
    rtlsdr_demod_write_reg(dev, 1, 0x93, 0xf0, 1);
    rtlsdr_demod_write_reg(dev, 1, 0x94, 0x0f, 1);

    /* disable AGC (en_dagc, bit 0) (this seems to have no effect) */
    rtlsdr_demod_write_reg(dev, 1, 0x11, 0x00, 1);

    /* disable RF and IF AGC loop */
    rtlsdr_demod_write_reg(dev, 1, 0x04, 0x00, 1);

    /* disable PID filter (enable_PID = 0) */
    rtlsdr_demod_write_reg(dev, 0, 0x61, 0x60, 1);

    /* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
    rtlsdr_demod_write_reg(dev, 0, 0x06, 0x80, 1);

    /* Enable Zero-IF mode (en_bbin bit), DC cancellation (en_dc_est),
     * IQ estimation/compensation (en_iq_comp, en_iq_est) */
    rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1b, 1);

    /* disable 4.096 MHz clock output on pin TP_CK0 */
    rtlsdr_demod_write_reg(dev, 0, 0x0d, 0x83, 1);
}

int rtlsdr_deinit_baseband(rtlsdr_dev_t *dev)
{
    int r = 0;

    if (!dev)
        return -1;

    if (dev->tuner && dev->tuner->exit)
    {
        rtlsdr_set_i2c_repeater(dev, 1);
        r = dev->tuner->exit(dev); /* deinitialize tuner */
        rtlsdr_set_i2c_repeater(dev, 0);
    }

    /* poweroff demodulator and ADCs */
    rtlsdr_write_reg(dev, SYSB, DEMOD_CTL, 0x20, 1);

    return r;
}

static int rtlsdr_set_if_freq(rtlsdr_dev_t *dev, uint32_t freq)
{
    uint32_t rtl_xtal;
    int32_t if_freq;
    uint8_t tmp;
    int r;

    if (!dev)
        return -1;

    /* read corrected clock value */
    if (rtlsdr_get_xtal_freq(dev, &rtl_xtal, NULL))
        return -2;

    if_freq = ((freq * TWO_POW(22)) / rtl_xtal) * (-1);

    tmp = (if_freq >> 16) & 0x3f;
    r = rtlsdr_demod_write_reg(dev, 1, 0x19, tmp, 1);
    tmp = (if_freq >> 8) & 0xff;
    r |= rtlsdr_demod_write_reg(dev, 1, 0x1a, tmp, 1);
    tmp = if_freq & 0xff;
    r |= rtlsdr_demod_write_reg(dev, 1, 0x1b, tmp, 1);

    return r;
}

int rtlsdr_set_sample_freq_correction(rtlsdr_dev_t *dev, int ppm)
{
    int r = 0;
    uint8_t tmp;
    int16_t offs = ppm * (-1) * TWO_POW(24) / 1000000;

    tmp = offs & 0xff;
    r |= rtlsdr_demod_write_reg(dev, 1, 0x3f, tmp, 1);
    tmp = (offs >> 8) & 0x3f;
    r |= rtlsdr_demod_write_reg(dev, 1, 0x3e, tmp, 1);

    return r;
}

int rtlsdr_set_xtal_freq(rtlsdr_dev_t *dev, uint32_t rtl_freq, uint32_t tuner_freq)
{
    int r = 0;

    if (!dev)
        return -1;

    if (rtl_freq > 0 &&
        (rtl_freq < MIN_RTL_XTAL_FREQ || rtl_freq > MAX_RTL_XTAL_FREQ))
        return -2;

    if (rtl_freq > 0 && dev->rtl_xtal != rtl_freq)
    {
        dev->rtl_xtal = rtl_freq;

        /* update xtal-dependent settings */
        if (dev->rate)
            r = rtlsdr_set_sample_rate(dev, dev->rate);
    }

    if (dev->tun_xtal != tuner_freq)
    {
        if (0 == tuner_freq)
            dev->tun_xtal = dev->rtl_xtal;
        else
            dev->tun_xtal = tuner_freq;

        /* read corrected clock value into e4k and r82xx structure */
        if (rtlsdr_get_xtal_freq(dev, NULL, &dev->e4k_s.vco.fosc) ||
            rtlsdr_get_xtal_freq(dev, NULL, &dev->r82xx_c.xtal))
            return -3;

        /* update xtal-dependent settings */
        if (dev->freq)
            r = rtlsdr_set_center_freq(dev, dev->freq);
    }

    return r;
}

int rtlsdr_get_xtal_freq(rtlsdr_dev_t *dev, uint32_t *rtl_freq, uint32_t *tuner_freq)
{
    if (!dev)
        return -1;

#define APPLY_PPM_CORR(val, ppm) (((val) * (1.0 + (ppm) / 1e6)))

    if (rtl_freq)
        *rtl_freq = (uint32_t)APPLY_PPM_CORR(dev->rtl_xtal, dev->corr);

    if (tuner_freq)
        *tuner_freq = (uint32_t)APPLY_PPM_CORR(dev->tun_xtal, dev->corr);

    return 0;
}

int rtlsdr_get_usb_strings(rtlsdr_dev_t *dev, char *manufact, char *product,
                           char *serial)
{
    assert(dev->driver_obj->dev_hdl != NULL);
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(dev->driver_obj->dev_hdl, &dev_info));
    const int buf_max = 256;

    if (dev_info.str_desc_manufacturer)
    {
        memset(manufact, 0, buf_max);
        esp_libusb_get_string_descriptor_ascii(dev_info.str_desc_manufacturer, manufact);
    }

    if (dev_info.str_desc_product)
    {
        memset(product, 0, buf_max);
        esp_libusb_get_string_descriptor_ascii(dev_info.str_desc_product, product);
    }

    if (dev_info.str_desc_serial_num)
    {
        memset(serial, 0, buf_max);
        esp_libusb_get_string_descriptor_ascii(dev_info.str_desc_serial_num, serial);
    }

    return 0;
}

int rtlsdr_write_eeprom(rtlsdr_dev_t *dev, uint8_t *data, uint8_t offset, uint16_t len)
{
    int r = 0;
    int i;
    uint8_t cmd[2];

    if (!dev)
        return -1;

    if ((len + offset) > 256)
        return -2;

    for (i = 0; i < len; i++)
    {
        cmd[0] = i + offset;
        r = rtlsdr_write_array(dev, IICB, EEPROM_ADDR, cmd, 1);
        r = rtlsdr_read_array(dev, IICB, EEPROM_ADDR, &cmd[1], 1);

        /* only write the byte if it differs */
        if (cmd[1] == data[i])
            continue;

        cmd[1] = data[i];
        r = rtlsdr_write_array(dev, IICB, EEPROM_ADDR, cmd, 2);
        if (r != sizeof(cmd))
            return -3;
        usleep(5000);
    }

    return 0;
}

int rtlsdr_read_eeprom(rtlsdr_dev_t *dev, uint8_t *data, uint8_t offset, uint16_t len)
{
    int r = 0;
    int i;

    if (!dev)
        return -1;

    if ((len + offset) > 256)
        return -2;

    r = rtlsdr_write_array(dev, IICB, EEPROM_ADDR, &offset, 1);
    if (r < 0)
        return -3;

    for (i = 0; i < len; i++)
    {
        r = rtlsdr_read_array(dev, IICB, EEPROM_ADDR, data + i, 1);

        if (r < 0)
            return -3;
    }

    return r;
}

int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t freq)
{
    int r = -1;

    if (!dev || !dev->tuner)
        return -1;

    if (dev->direct_sampling)
    {
        r = rtlsdr_set_if_freq(dev, freq);
    }
    else if (dev->tuner && dev->tuner->set_freq)
    {
        rtlsdr_set_i2c_repeater(dev, 1);
        r = dev->tuner->set_freq(dev, freq - dev->offs_freq);
        rtlsdr_set_i2c_repeater(dev, 0);
    }

    if (!r)
        dev->freq = freq;
    else
        dev->freq = 0;

    return r;
}

uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev)
{
    if (!dev)
        return 0;

    return dev->freq;
}

int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm)
{
    int r = 0;
    rtlsdr_set_i2c_repeater(dev, 0);

    if (!dev)
        return -1;

    if (dev->corr == ppm)
        return -2;

    dev->corr = ppm;

    r |= rtlsdr_set_sample_freq_correction(dev, ppm);

    /* read corrected clock value into e4k and r82xx structure */
    if (rtlsdr_get_xtal_freq(dev, NULL, &dev->e4k_s.vco.fosc) ||
        rtlsdr_get_xtal_freq(dev, NULL, &dev->r82xx_c.xtal))
        return -3;

    if (dev->freq) /* retune to apply new correction value */
        r |= rtlsdr_set_center_freq(dev, dev->freq);

    return r;
}

int rtlsdr_get_freq_correction(rtlsdr_dev_t *dev)
{
    if (!dev)
        return 0;

    return dev->corr;
}

enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t *dev)
{
    if (!dev)
        return RTLSDR_TUNER_UNKNOWN;

    return dev->tuner_type;
}

int rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains)
{
    /* all gain values are expressed in tenths of a dB */
    const int e4k_gains[] = {-10, 15, 40, 65, 90, 115, 140, 165, 190, 215,
                             240, 290, 340, 420};
    const int fc0012_gains[] = {-99, -40, 71, 179, 192};
    const int fc0013_gains[] = {-99, -73, -65, -63, -60, -58, -54, 58, 61,
                                63, 65, 67, 68, 70, 71, 179, 181, 182,
                                184, 186, 188, 191, 197};
    const int fc2580_gains[] = {0 /* no gain values */};
    const int r82xx_gains[] = {0, 9, 14, 27, 37, 77, 87, 125, 144, 157,
                               166, 197, 207, 229, 254, 280, 297, 328,
                               338, 364, 372, 386, 402, 421, 434, 439,
                               445, 480, 496};
    const int unknown_gains[] = {0 /* no gain values */};

    const int *ptr = NULL;
    int len = 0;

    if (!dev)
        return -1;

    switch (dev->tuner_type)
    {
    case RTLSDR_TUNER_E4000:
        ptr = e4k_gains;
        len = sizeof(e4k_gains);
        break;
    case RTLSDR_TUNER_FC0012:
        ptr = fc0012_gains;
        len = sizeof(fc0012_gains);
        break;
    case RTLSDR_TUNER_FC0013:
        ptr = fc0013_gains;
        len = sizeof(fc0013_gains);
        break;
    case RTLSDR_TUNER_FC2580:
        ptr = fc2580_gains;
        len = sizeof(fc2580_gains);
        break;
    case RTLSDR_TUNER_R820T:
    case RTLSDR_TUNER_R828D:
        ptr = r82xx_gains;
        len = sizeof(r82xx_gains);
        break;
    default:
        ptr = unknown_gains;
        len = sizeof(unknown_gains);
        break;
    }

    if (!gains)
    { /* no buffer provided, just return the count */
        return len / sizeof(int);
    }
    else
    {
        if (len)
            memcpy(gains, ptr, len);

        return len / sizeof(int);
    }
}

int rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t *dev, uint32_t bw)
{
    int r = 0;

    if (!dev || !dev->tuner)
        return -1;

    if (dev->tuner->set_bw)
    {
        rtlsdr_set_i2c_repeater(dev, 1);
        r = dev->tuner->set_bw(dev, bw > 0 ? bw : dev->rate);
        rtlsdr_set_i2c_repeater(dev, 0);
        if (r)
            return r;
        dev->bw = bw;
    }
    return r;
}

int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain)
{
    int r = 0;

    if (!dev || !dev->tuner)
        return -1;

    if (dev->tuner->set_gain)
    {
        rtlsdr_set_i2c_repeater(dev, 1);
        r = dev->tuner->set_gain((void *)dev, gain);
        rtlsdr_set_i2c_repeater(dev, 0);
    }

    if (!r)
        dev->gain = gain;
    else
        dev->gain = 0;

    return r;
}

int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev)
{
    if (!dev)
        return 0;

    return dev->gain;
}

int rtlsdr_set_tuner_if_gain(rtlsdr_dev_t *dev, int stage, int gain)
{
    int r = 0;

    if (!dev || !dev->tuner)
        return -1;

    if (dev->tuner->set_if_gain)
    {
        rtlsdr_set_i2c_repeater(dev, 1);
        r = dev->tuner->set_if_gain(dev, stage, gain);
        rtlsdr_set_i2c_repeater(dev, 0);
    }

    return r;
}

int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int mode)
{
    int r = 0;

    if (!dev || !dev->tuner)
        return -1;

    if (dev->tuner->set_gain_mode)
    {
        rtlsdr_set_i2c_repeater(dev, 1);
        r = dev->tuner->set_gain_mode((void *)dev, mode);
        rtlsdr_set_i2c_repeater(dev, 0);
    }

    return r;
}

int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t samp_rate)
{
    int r = 0;
    uint16_t tmp;
    uint32_t rsamp_ratio, real_rsamp_ratio;
    double real_rate;

    if (!dev)
        return -1;

    /* check if the rate is supported by the resampler */
    if ((samp_rate <= 225000) || (samp_rate > 3200000) ||
        ((samp_rate > 300000) && (samp_rate <= 900000)))
    {
        fprintf(stderr, "Invalid sample rate: %lu Hz\n", samp_rate);
        return -EINVAL;
    }

    rsamp_ratio = (dev->rtl_xtal * TWO_POW(22)) / samp_rate;
    rsamp_ratio &= 0x0ffffffc;

    real_rsamp_ratio = rsamp_ratio | ((rsamp_ratio & 0x08000000) << 1);
    real_rate = (dev->rtl_xtal * TWO_POW(22)) / real_rsamp_ratio;

    if (((double)samp_rate) != real_rate)
        fprintf(stderr, "Exact sample rate is: %f Hz\n", real_rate);

    dev->rate = (uint32_t)real_rate;

    if (dev->tuner && dev->tuner->set_bw)
    {
        rtlsdr_set_i2c_repeater(dev, 1);
        dev->tuner->set_bw(dev, dev->bw > 0 ? dev->bw : dev->rate);
        rtlsdr_set_i2c_repeater(dev, 0);
    }

    tmp = (rsamp_ratio >> 16);
    r |= rtlsdr_demod_write_reg(dev, 1, 0x9f, tmp, 2);
    tmp = rsamp_ratio & 0xffff;
    r |= rtlsdr_demod_write_reg(dev, 1, 0xa1, tmp, 2);

    r |= rtlsdr_set_sample_freq_correction(dev, dev->corr);

    /* reset demod (bit 3, soft_rst) */
    r |= rtlsdr_demod_write_reg(dev, 1, 0x01, 0x14, 1);
    r |= rtlsdr_demod_write_reg(dev, 1, 0x01, 0x10, 1);

    /* recalculate offset frequency if offset tuning is enabled */
    if (dev->offs_freq)
        rtlsdr_set_offset_tuning(dev, 1);

    return r;
}

uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev)
{
    if (!dev)
        return 0;

    return dev->rate;
}

int rtlsdr_set_testmode(rtlsdr_dev_t *dev, int on)
{
    if (!dev)
        return -1;

    return rtlsdr_demod_write_reg(dev, 0, 0x19, on ? 0x03 : 0x05, 1);
}

int rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on)
{
    if (!dev)
        return -1;

    return rtlsdr_demod_write_reg(dev, 0, 0x19, on ? 0x25 : 0x05, 1);
}

int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on)
{
    int r = 0;

    if (!dev)
        return -1;

    if (on)
    {
        if (dev->tuner && dev->tuner->exit)
        {
            rtlsdr_set_i2c_repeater(dev, 1);
            r = dev->tuner->exit(dev);
            rtlsdr_set_i2c_repeater(dev, 0);
        }

        /* disable Zero-IF mode */
        r |= rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1a, 1);

        /* disable spectrum inversion */
        r |= rtlsdr_demod_write_reg(dev, 1, 0x15, 0x00, 1);

        /* only enable In-phase ADC input */
        r |= rtlsdr_demod_write_reg(dev, 0, 0x08, 0x4d, 1);

        /* swap I and Q ADC, this allows to select between two inputs */
        r |= rtlsdr_demod_write_reg(dev, 0, 0x06, (on > 1) ? 0x90 : 0x80, 1);

        fprintf(stderr, "Enabled direct sampling mode, input %i\n", on);
        dev->direct_sampling = on;
    }
    else
    {
        if (dev->tuner && dev->tuner->init)
        {
            rtlsdr_set_i2c_repeater(dev, 1);
            r |= dev->tuner->init(dev);
            rtlsdr_set_i2c_repeater(dev, 0);
        }

        if ((dev->tuner_type == RTLSDR_TUNER_R820T) ||
            (dev->tuner_type == RTLSDR_TUNER_R828D))
        {
            r |= rtlsdr_set_if_freq(dev, R82XX_IF_FREQ);

            /* enable spectrum inversion */
            r |= rtlsdr_demod_write_reg(dev, 1, 0x15, 0x01, 1);
        }
        else
        {
            r |= rtlsdr_set_if_freq(dev, 0);

            /* enable In-phase + Quadrature ADC input */
            r |= rtlsdr_demod_write_reg(dev, 0, 0x08, 0xcd, 1);

            /* Enable Zero-IF mode */
            r |= rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1b, 1);
        }

        /* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
        r |= rtlsdr_demod_write_reg(dev, 0, 0x06, 0x80, 1);

        fprintf(stderr, "Disabled direct sampling mode\n");
        dev->direct_sampling = 0;
    }

    r |= rtlsdr_set_center_freq(dev, dev->freq);

    return r;
}

int rtlsdr_get_direct_sampling(rtlsdr_dev_t *dev)
{
    if (!dev)
        return -1;

    return dev->direct_sampling;
}

int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on)
{
    int r = 0;
    int bw;

    if (!dev)
        return -1;

    if ((dev->tuner_type == RTLSDR_TUNER_R820T) ||
        (dev->tuner_type == RTLSDR_TUNER_R828D))
        return -2;

    if (dev->direct_sampling)
        return -3;

    /* based on keenerds 1/f noise measurements */
    dev->offs_freq = on ? ((dev->rate / 2) * 170 / 100) : 0;
    r |= rtlsdr_set_if_freq(dev, dev->offs_freq);

    if (dev->tuner && dev->tuner->set_bw)
    {
        rtlsdr_set_i2c_repeater(dev, 1);
        if (on)
        {
            bw = 2 * dev->offs_freq;
        }
        else if (dev->bw > 0)
        {
            bw = dev->bw;
        }
        else
        {
            bw = dev->rate;
        }
        dev->tuner->set_bw(dev, bw);
        rtlsdr_set_i2c_repeater(dev, 0);
    }

    if (dev->freq > dev->offs_freq)
        r |= rtlsdr_set_center_freq(dev, dev->freq);

    return r;
}

int rtlsdr_get_offset_tuning(rtlsdr_dev_t *dev)
{
    if (!dev)
        return -1;

    return (dev->offs_freq) ? 1 : 0;
}

static rtlsdr_dongle_t *find_known_device(uint16_t vid, uint16_t pid)
{
    unsigned int i;
    rtlsdr_dongle_t *device = NULL;

    for (i = 0; i < sizeof(known_devices) / sizeof(rtlsdr_dongle_t); i++)
    {
        if (known_devices[i].vid == vid && known_devices[i].pid == pid)
        {
            device = &known_devices[i];
            break;
        }
    }

    return device;
}

void esp_action_get_dev_desc(rtlsdr_dev_t *dev)
{
    assert(dev->driver_obj->dev_hdl != NULL);
    ESP_LOGI(TAG_ADSB, "Getting config descriptor");
    const usb_config_desc_t *config_desc;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(dev->driver_obj->dev_hdl, &config_desc));
    usb_print_config_descriptor(config_desc, NULL);
}

int rtlsdr_check_dongle_model(void *dev_, const char *manufact_check, const char *product_check)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)dev_;
    if (!dev) return 0;
    if (strcmp(dev->manufact, manufact_check) == 0 &&
        strcmp(dev->product,  product_check)  == 0)
        return 1;
    return 0;
}

int rtlsdr_open(rtlsdr_dev_t **out_dev, uint8_t index, usb_host_client_handle_t client_hdl)
{
    int r;
    rtlsdr_dev_t *dev = NULL;
    uint8_t reg;

    dev = malloc(sizeof(rtlsdr_dev_t));
    class_driver_t *driver_obj = calloc(1, sizeof(class_driver_t));
    if (NULL == dev)
        return -1; // Usually -ENOMEM

    memset(dev, 0, sizeof(rtlsdr_dev_t));
    memcpy(dev->fir, fir_default, sizeof(fir_default));

    dev->dev_lost = 1;

    driver_obj->client_hdl = client_hdl;
    ESP_ERROR_CHECK(usb_host_device_open(driver_obj->client_hdl, index, &driver_obj->dev_hdl));
    dev->driver_obj = driver_obj;
    init_adsb_dev();
    
    // ---> CLAIM INTERFACE FIRST <---
    ESP_ERROR_CHECK(usb_host_interface_claim(dev->driver_obj->client_hdl, dev->driver_obj->dev_hdl, 0, 0));

    /* perform a dummy write, if it fails, it's safe to ignore now */
    if (rtlsdr_write_reg(dev, USBB, USB_SYSCTL, 0x09, 1) < 0)
    {
        printf("Dummy write failed. Device might need reset, continuing anyway...\n");
    }
    
    dev->rtl_xtal = 28800000; // DEF_RTL_XTAL_FREQ

    rtlsdr_init_baseband(dev);
    dev->dev_lost = 0;

    /* Probe tuners */
    rtlsdr_set_i2c_repeater(dev, 1);
    
    // reg = rtlsdr_i2c_read_reg(dev, E4K_I2C_ADDR, E4K_CHECK_ADDR);
    // fprintf(stderr, "rtlsdr_i2c_read_reg E4K_I2C_ADDR setting done\n");
    // if (reg == E4K_CHECK_VAL)
    // {
    //     fprintf(stderr, "Found Elonics E4000 tuner\n");
    //     dev->tuner_type = RTLSDR_TUNER_E4000;
    //     goto found;
    // }

    // reg = rtlsdr_i2c_read_reg(dev, FC0013_I2C_ADDR, FC0013_CHECK_ADDR);
    // fprintf(stderr, "rtlsdr_i2c_read_reg FC0013_I2C_ADDR setting done\n");
    // if (reg == FC0013_CHECK_VAL)
    // {
    //     fprintf(stderr, "Found Fitipower FC0013 tuner\n");
    //     dev->tuner_type = RTLSDR_TUNER_FC0013;
    //     goto found;
    // }

    reg = rtlsdr_i2c_read_reg(dev, R820T_I2C_ADDR, R82XX_CHECK_ADDR);
    ESP_LOGI(TAG_ADSB, "rtl device number %d", reg);
    fprintf(stderr, "rtlsdr_i2c_read_reg R82XX_CHECK_ADDR setting done\n");
    if (reg == R82XX_CHECK_VAL)
    {
        fprintf(stderr, "Found Rafael Micro R820T tuner\n");
        dev->tuner_type = RTLSDR_TUNER_R820T;
        goto found;
    }

    reg = rtlsdr_i2c_read_reg(dev, R828D_I2C_ADDR, R82XX_CHECK_ADDR);
    fprintf(stderr, "rtlsdr_i2c_read_reg R828D_I2C_ADDR setting done\n");
    if (reg == R82XX_CHECK_VAL)
    {
        fprintf(stderr, "Found Rafael Micro R828D tuner\n");
        dev->tuner_type = RTLSDR_TUNER_R828D;
        goto found;
    }

    // /* initialise GPIOs */
    rtlsdr_set_gpio_output(dev, 4);

    // /* reset tuner before probing */
    rtlsdr_set_gpio_bit(dev, 4, 1);
    rtlsdr_set_gpio_bit(dev, 4, 0);

    // reg = rtlsdr_i2c_read_reg(dev, FC2580_I2C_ADDR, FC2580_CHECK_ADDR);
    // if ((reg & 0x7f) == FC2580_CHECK_VAL)
    // {
    //     fprintf(stderr, "Found FCI 2580 tuner\n");
    //     dev->tuner_type = RTLSDR_TUNER_FC2580;
    //     goto found;
    // }

    // reg = rtlsdr_i2c_read_reg(dev, FC0012_I2C_ADDR, FC0012_CHECK_ADDR);
    // if (reg == FC0012_CHECK_VAL)
    // {
    //     fprintf(stderr, "Found Fitipower FC0012 tuner\n");
    //     rtlsdr_set_gpio_output(dev, 6);
    //     dev->tuner_type = RTLSDR_TUNER_FC0012;
    //     goto found;
    // }

found:
    /* use the rtl clock value by default */
    dev->tun_xtal = dev->rtl_xtal;
    dev->tuner = &tuners[dev->tuner_type];

    switch (dev->tuner_type)
    {
    case RTLSDR_TUNER_R828D:
       // dev->tun_xtal = R828D_XTAL_FREQ;
	   dev->tun_xtal = dev->rtl_xtal;
        /* fall-through */
    case RTLSDR_TUNER_R820T:
        /* disable Zero-IF mode */
        rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1a, 1);

        /* only enable In-phase ADC input */
        rtlsdr_demod_write_reg(dev, 0, 0x08, 0x4d, 1);

        /* the R82XX use 3.57 MHz IF for the DVB-T 6 MHz mode, and
         * 4.57 MHz for the 8 MHz mode */
        rtlsdr_set_if_freq(dev, R82XX_IF_FREQ);

        /* enable spectrum inversion */
        rtlsdr_demod_write_reg(dev, 1, 0x15, 0x01, 1);
        break;
    case RTLSDR_TUNER_UNKNOWN:
        fprintf(stderr, "No supported tuner found\n");
        rtlsdr_set_direct_sampling(dev, 1);
        break;
    default:
        break;
    }

    if (dev->tuner->init)
        r = dev->tuner->init(dev);

    rtlsdr_set_i2c_repeater(dev, 0);

    {
        char tmp_serial[256];
        memset(dev->manufact, 0, sizeof(dev->manufact));
        memset(dev->product,  0, sizeof(dev->product));
        memset(tmp_serial,    0, sizeof(tmp_serial));
        rtlsdr_get_usb_strings(dev, dev->manufact, dev->product, tmp_serial);
        ESP_LOGI(TAG_ADSB, "USB strings: manufact='%s' product='%s'",
                 dev->manufact, dev->product);
        if (strcmp(dev->manufact, "RTLSDRBlog") == 0 &&
            strcmp(dev->product,  "Blog V4")   == 0) {
            ESP_LOGI(TAG_ADSB, "*** RTL-SDR Blog V4 detected - triplexer active ***");
        }
    }

    *out_dev = dev;

    return 0;
}

int rtlsdr_close(rtlsdr_dev_t *dev)
{
    //     if (!dev)
    //         return -1;

    //     if (!dev->dev_lost)
    //     {
    //         /* block until all async operations have been completed (if any) */
    //         while (RTLSDR_INACTIVE != dev->async_status)
    //         {
    //             usleep(1000);
    //         }

    //         rtlsdr_deinit_baseband(dev);
    //     }

    //     libusb_release_interface(dev->devh, 0);

    // #ifdef DETACH_KERNEL_DRIVER
    //     if (dev->driver_active)
    //     {
    //         if (!libusb_attach_kernel_driver(dev->devh, 0))
    //             fprintf(stderr, "Reattached kernel driver\n");
    //         else
    //             fprintf(stderr, "Reattaching kernel driver failed!\n");
    //     }
    // #endif

    //     libusb_close(dev->devh);

    //     libusb_exit(dev->ctx);

    //     free(dev);

    return 0;
}

int rtlsdr_reset_buffer(rtlsdr_dev_t *dev)
{
    if (!dev)
        return -1;

    rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x1002, 2);
    rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x0000, 2);

    return 0;
}

int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read)
{
    if (!dev)
        return -1;

    return esp_libusb_bulk_transfer(dev->driver_obj, 0x81, buf, len, n_read, BULK_TIMEOUT);
}

int rtlsdr_cancel_async(rtlsdr_dev_t *dev)
{
    if (!dev)
        return -1;

    /* if streaming, try to cancel gracefully */
    if (RTLSDR_RUNNING == dev->async_status)
    {
        dev->async_status = RTLSDR_CANCELING;
        dev->async_cancel = 1;
        return 0;
    }

    /* if called while in pending state, change the state forcefully */
#if 0
	if (RTLSDR_INACTIVE != dev->async_status) {
		dev->async_status = RTLSDR_INACTIVE;
		return 0;
	}
#endif
    return -2;
}

uint32_t rtlsdr_get_tuner_clock(void *dev)
{
    uint32_t tuner_freq;

    if (!dev)
        return 0;

    /* read corrected clock value */
    if (rtlsdr_get_xtal_freq((rtlsdr_dev_t *)dev, NULL, &tuner_freq))
        return 0;

    return tuner_freq;
}

int rtlsdr_i2c_write_fn(void *dev, uint8_t addr, uint8_t *buf, int len)
{
    if (dev)
        return rtlsdr_i2c_write(((rtlsdr_dev_t *)dev), addr, buf, len);

    return -1;
}

int rtlsdr_i2c_read_fn(void *dev, uint8_t addr, uint8_t *buf, int len)
{
    if (dev)
        return rtlsdr_i2c_read(((rtlsdr_dev_t *)dev), addr, buf, len);

    return -1;
}

int rtlsdr_set_bias_tee_gpio(rtlsdr_dev_t *dev, int gpio, int on)
{
    if (!dev)
        return -1;

    rtlsdr_set_gpio_output(dev, gpio);
    rtlsdr_set_gpio_bit(dev, gpio, on);

    return 0;
}

int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on)
{
    return rtlsdr_set_bias_tee_gpio(dev, 0, on);
}