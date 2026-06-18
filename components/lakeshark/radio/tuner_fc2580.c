
#include <stdint.h>

#include "rtlsdr_i2c.h"
#include "tuner_fc2580.h"

#define CRYSTAL_FREQ 16384000

fc2580_fci_result_type fc2580_i2c_write(void *pTuner, unsigned char reg, unsigned char val)
{
    uint8_t data[2];

    data[0] = reg;
    data[1] = val;

    if (rtlsdr_i2c_write_fn(pTuner, FC2580_I2C_ADDR, data, 2) < 0)
        return FC2580_FCI_FAIL;

    return FC2580_FCI_SUCCESS;
}

fc2580_fci_result_type fc2580_i2c_read(void *pTuner, unsigned char reg, unsigned char *read_data)
{
    uint8_t data = reg;

    if (rtlsdr_i2c_write_fn(pTuner, FC2580_I2C_ADDR, &data, 1) < 0)
        return FC2580_FCI_FAIL;

    if (rtlsdr_i2c_read_fn(pTuner, FC2580_I2C_ADDR, &data, 1) < 0)
        return FC2580_FCI_FAIL;

    *read_data = data;

    return FC2580_FCI_SUCCESS;
}

int fc2580_Initialize(
    void *pTuner)
{
    int AgcMode;
    unsigned int CrystalFreqKhz;

    AgcMode = FC2580_AGC_EXTERNAL;

    CrystalFreqKhz = (unsigned int)((CRYSTAL_FREQ + 500) / 1000);

    if (fc2580_set_init(pTuner, AgcMode, CrystalFreqKhz) != FC2580_FCI_SUCCESS)
        goto error_status_initialize_tuner;

    return FUNCTION_SUCCESS;

error_status_initialize_tuner:
    return FUNCTION_ERROR;
}

int fc2580_SetRfFreqHz(
    void *pTuner,
    unsigned long RfFreqHz)
{
    unsigned int RfFreqKhz;
    unsigned int CrystalFreqKhz;

    RfFreqKhz = (unsigned int)((RfFreqHz + 500) / 1000);
    CrystalFreqKhz = (unsigned int)((CRYSTAL_FREQ + 500) / 1000);

    if (fc2580_set_freq(pTuner, RfFreqKhz, CrystalFreqKhz) != FC2580_FCI_SUCCESS)
        goto error_status_set_tuner_rf_frequency;

    return FUNCTION_SUCCESS;

error_status_set_tuner_rf_frequency:
    return FUNCTION_ERROR;
}

int fc2580_SetBandwidthMode(
    void *pTuner,
    int BandwidthMode)
{
    unsigned int CrystalFreqKhz;

    CrystalFreqKhz = (unsigned int)((CRYSTAL_FREQ + 500) / 1000);

    if (fc2580_set_filter(pTuner, (unsigned char)BandwidthMode, CrystalFreqKhz) != FC2580_FCI_SUCCESS)
        goto error_status_set_tuner_bandwidth_mode;

    return FUNCTION_SUCCESS;

error_status_set_tuner_bandwidth_mode:
    return FUNCTION_ERROR;
}

void fc2580_wait_msec(void *pTuner, int a)
{

    return;
}

fc2580_fci_result_type fc2580_set_init(void *pTuner, int ifagc_mode, unsigned int freq_xtal)
{
    fc2580_fci_result_type result = FC2580_FCI_SUCCESS;

    result &= fc2580_i2c_write(pTuner, 0x00, 0x00);
    result &= fc2580_i2c_write(pTuner, 0x12, 0x86);
    result &= fc2580_i2c_write(pTuner, 0x14, 0x5C);
    result &= fc2580_i2c_write(pTuner, 0x16, 0x3C);
    result &= fc2580_i2c_write(pTuner, 0x1F, 0xD2);
    result &= fc2580_i2c_write(pTuner, 0x09, 0xD7);
    result &= fc2580_i2c_write(pTuner, 0x0B, 0xD5);
    result &= fc2580_i2c_write(pTuner, 0x0C, 0x32);
    result &= fc2580_i2c_write(pTuner, 0x0E, 0x43);
    result &= fc2580_i2c_write(pTuner, 0x21, 0x0A);
    result &= fc2580_i2c_write(pTuner, 0x22, 0x82);
    if (ifagc_mode == 1)
    {
        result &= fc2580_i2c_write(pTuner, 0x45, 0x10);
        result &= fc2580_i2c_write(pTuner, 0x4C, 0x00);
    }
    else if (ifagc_mode == 2)
    {
        result &= fc2580_i2c_write(pTuner, 0x45, 0x20);
        result &= fc2580_i2c_write(pTuner, 0x4C, 0x02);
    }
    result &= fc2580_i2c_write(pTuner, 0x3F, 0x88);
    result &= fc2580_i2c_write(pTuner, 0x02, 0x0E);
    result &= fc2580_i2c_write(pTuner, 0x58, 0x14);
    result &= fc2580_set_filter(pTuner, 8, freq_xtal);

    return result;
}

fc2580_fci_result_type fc2580_set_freq(void *pTuner, unsigned int f_lo, unsigned int freq_xtal)
{
    unsigned int f_diff, f_diff_shifted, n_val, k_val;
    unsigned int f_vco, r_val, f_comp;
    unsigned char pre_shift_bits = 4;
    unsigned char data_0x18;
    unsigned char data_0x02 = (USE_EXT_CLK << 5) | 0x0E;

    fc2580_band_type band = (f_lo > 1000000) ? FC2580_L_BAND : (f_lo > 400000) ? FC2580_UHF_BAND
                                                                               : FC2580_VHF_BAND;

    fc2580_fci_result_type result = FC2580_FCI_SUCCESS;

    f_vco = (band == FC2580_UHF_BAND) ? f_lo * 4 : ((band == FC2580_L_BAND) ? f_lo * 2 : f_lo * 12);
    r_val = (f_vco >= 2 * 76 * freq_xtal) ? 1 : (f_vco >= 76 * freq_xtal) ? 2
                                                                          : 4;
    f_comp = freq_xtal / r_val;
    n_val = (f_vco / 2) / f_comp;

    f_diff = f_vco - 2 * f_comp * n_val;
    f_diff_shifted = f_diff << (20 - pre_shift_bits);
    k_val = f_diff_shifted / ((2 * f_comp) >> pre_shift_bits);

    if (f_diff_shifted - k_val * ((2 * f_comp) >> pre_shift_bits) >= (f_comp >> pre_shift_bits))
        k_val = k_val + 1;

    if (f_vco >= BORDER_FREQ)
        data_0x02 = data_0x02 | 0x08;
    else
        data_0x02 = data_0x02 & 0xF7;

    switch (band)
    {
    case FC2580_UHF_BAND:
        data_0x02 = (data_0x02 & 0x3F);

        result &= fc2580_i2c_write(pTuner, 0x25, 0xF0);
        result &= fc2580_i2c_write(pTuner, 0x27, 0x77);
        result &= fc2580_i2c_write(pTuner, 0x28, 0x53);
        result &= fc2580_i2c_write(pTuner, 0x29, 0x60);
        result &= fc2580_i2c_write(pTuner, 0x30, 0x09);
        result &= fc2580_i2c_write(pTuner, 0x50, 0x8C);
        result &= fc2580_i2c_write(pTuner, 0x53, 0x50);

        if (f_lo < 538000)
            result &= fc2580_i2c_write(pTuner, 0x5F, 0x13);
        else
            result &= fc2580_i2c_write(pTuner, 0x5F, 0x15);

        if (f_lo < 538000)
        {
            result &= fc2580_i2c_write(pTuner, 0x61, 0x07);
            result &= fc2580_i2c_write(pTuner, 0x62, 0x06);
            result &= fc2580_i2c_write(pTuner, 0x67, 0x06);
            result &= fc2580_i2c_write(pTuner, 0x68, 0x08);
            result &= fc2580_i2c_write(pTuner, 0x69, 0x10);
            result &= fc2580_i2c_write(pTuner, 0x6A, 0x12);
        }
        else if (f_lo < 794000)
        {
            result &= fc2580_i2c_write(pTuner, 0x61, 0x03);
            result &= fc2580_i2c_write(pTuner, 0x62, 0x03);
            result &= fc2580_i2c_write(pTuner, 0x67, 0x03);
            result &= fc2580_i2c_write(pTuner, 0x68, 0x05);
            result &= fc2580_i2c_write(pTuner, 0x69, 0x0C);
            result &= fc2580_i2c_write(pTuner, 0x6A, 0x0E);
        }
        else
        {
            result &= fc2580_i2c_write(pTuner, 0x61, 0x07);
            result &= fc2580_i2c_write(pTuner, 0x62, 0x06);
            result &= fc2580_i2c_write(pTuner, 0x67, 0x07);
            result &= fc2580_i2c_write(pTuner, 0x68, 0x09);
            result &= fc2580_i2c_write(pTuner, 0x69, 0x10);
            result &= fc2580_i2c_write(pTuner, 0x6A, 0x12);
        }

        result &= fc2580_i2c_write(pTuner, 0x63, 0x15);

        result &= fc2580_i2c_write(pTuner, 0x6B, 0x0B);
        result &= fc2580_i2c_write(pTuner, 0x6C, 0x0C);
        result &= fc2580_i2c_write(pTuner, 0x6D, 0x78);
        result &= fc2580_i2c_write(pTuner, 0x6E, 0x32);
        result &= fc2580_i2c_write(pTuner, 0x6F, 0x14);
        result &= fc2580_set_filter(pTuner, 8, freq_xtal);
        break;
    case FC2580_VHF_BAND:
        data_0x02 = (data_0x02 & 0x3F) | 0x80;
        result &= fc2580_i2c_write(pTuner, 0x27, 0x77);
        result &= fc2580_i2c_write(pTuner, 0x28, 0x33);
        result &= fc2580_i2c_write(pTuner, 0x29, 0x40);
        result &= fc2580_i2c_write(pTuner, 0x30, 0x09);
        result &= fc2580_i2c_write(pTuner, 0x50, 0x8C);
        result &= fc2580_i2c_write(pTuner, 0x53, 0x50);
        result &= fc2580_i2c_write(pTuner, 0x5F, 0x0F);
        result &= fc2580_i2c_write(pTuner, 0x61, 0x07);
        result &= fc2580_i2c_write(pTuner, 0x62, 0x00);
        result &= fc2580_i2c_write(pTuner, 0x63, 0x15);
        result &= fc2580_i2c_write(pTuner, 0x67, 0x03);
        result &= fc2580_i2c_write(pTuner, 0x68, 0x05);
        result &= fc2580_i2c_write(pTuner, 0x69, 0x10);
        result &= fc2580_i2c_write(pTuner, 0x6A, 0x12);
        result &= fc2580_i2c_write(pTuner, 0x6B, 0x08);
        result &= fc2580_i2c_write(pTuner, 0x6C, 0x0A);
        result &= fc2580_i2c_write(pTuner, 0x6D, 0x78);
        result &= fc2580_i2c_write(pTuner, 0x6E, 0x32);
        result &= fc2580_i2c_write(pTuner, 0x6F, 0x54);
        result &= fc2580_set_filter(pTuner, 7, freq_xtal);
        break;
    case FC2580_L_BAND:
        data_0x02 = (data_0x02 & 0x3F) | 0x40;
        result &= fc2580_i2c_write(pTuner, 0x2B, 0x70);
        result &= fc2580_i2c_write(pTuner, 0x2C, 0x37);
        result &= fc2580_i2c_write(pTuner, 0x2D, 0xE7);
        result &= fc2580_i2c_write(pTuner, 0x30, 0x09);
        result &= fc2580_i2c_write(pTuner, 0x44, 0x20);
        result &= fc2580_i2c_write(pTuner, 0x50, 0x8C);
        result &= fc2580_i2c_write(pTuner, 0x53, 0x50);
        result &= fc2580_i2c_write(pTuner, 0x5F, 0x0F);
        result &= fc2580_i2c_write(pTuner, 0x61, 0x0F);
        result &= fc2580_i2c_write(pTuner, 0x62, 0x00);
        result &= fc2580_i2c_write(pTuner, 0x63, 0x13);
        result &= fc2580_i2c_write(pTuner, 0x67, 0x00);
        result &= fc2580_i2c_write(pTuner, 0x68, 0x02);
        result &= fc2580_i2c_write(pTuner, 0x69, 0x0C);
        result &= fc2580_i2c_write(pTuner, 0x6A, 0x0E);
        result &= fc2580_i2c_write(pTuner, 0x6B, 0x08);
        result &= fc2580_i2c_write(pTuner, 0x6C, 0x0A);
        result &= fc2580_i2c_write(pTuner, 0x6D, 0xA0);
        result &= fc2580_i2c_write(pTuner, 0x6E, 0x50);
        result &= fc2580_i2c_write(pTuner, 0x6F, 0x14);
        result &= fc2580_set_filter(pTuner, 1, freq_xtal);
        break;
    default:
        break;
    }

    if (freq_xtal >= 28000)
        result &= fc2580_i2c_write(pTuner, 0x4B, 0x22);

    result &= fc2580_i2c_write(pTuner, 0x02, data_0x02);
    data_0x18 = ((r_val == 1) ? 0x00 : ((r_val == 2) ? 0x10 : 0x20)) + (unsigned char)(k_val >> 16);
    result &= fc2580_i2c_write(pTuner, 0x18, data_0x18);
    result &= fc2580_i2c_write(pTuner, 0x1A, (unsigned char)(k_val >> 8));
    result &= fc2580_i2c_write(pTuner, 0x1B, (unsigned char)(k_val));
    result &= fc2580_i2c_write(pTuner, 0x1C, (unsigned char)(n_val));

    if (band == FC2580_UHF_BAND)
        result &= fc2580_i2c_write(pTuner, 0x2D, (f_lo <= (unsigned int)794000) ? 0x9F : 0x8F);

    return result;
}

fc2580_fci_result_type fc2580_set_filter(void *pTuner, unsigned char filter_bw, unsigned int freq_xtal)
{
    unsigned char cal_mon = 0, i;
    fc2580_fci_result_type result = FC2580_FCI_SUCCESS;

    if (filter_bw == 1)
    {
        result &= fc2580_i2c_write(pTuner, 0x36, 0x1C);
        result &= fc2580_i2c_write(pTuner, 0x37, (unsigned char)(4151 * freq_xtal / 1000000));
        result &= fc2580_i2c_write(pTuner, 0x39, 0x00);
        result &= fc2580_i2c_write(pTuner, 0x2E, 0x09);
    }
    if (filter_bw == 6)
    {
        result &= fc2580_i2c_write(pTuner, 0x36, 0x18);
        result &= fc2580_i2c_write(pTuner, 0x37, (unsigned char)(4400 * freq_xtal / 1000000));
        result &= fc2580_i2c_write(pTuner, 0x39, 0x00);
        result &= fc2580_i2c_write(pTuner, 0x2E, 0x09);
    }
    else if (filter_bw == 7)
    {
        result &= fc2580_i2c_write(pTuner, 0x36, 0x18);
        result &= fc2580_i2c_write(pTuner, 0x37, (unsigned char)(3910 * freq_xtal / 1000000));
        result &= fc2580_i2c_write(pTuner, 0x39, 0x80);
        result &= fc2580_i2c_write(pTuner, 0x2E, 0x09);
    }
    else if (filter_bw == 8)
    {
        result &= fc2580_i2c_write(pTuner, 0x36, 0x18);
        result &= fc2580_i2c_write(pTuner, 0x37, (unsigned char)(3300 * freq_xtal / 1000000));
        result &= fc2580_i2c_write(pTuner, 0x39, 0x80);
        result &= fc2580_i2c_write(pTuner, 0x2E, 0x09);
    }

    for (i = 0; i < 5; i++)
    {
        fc2580_wait_msec(pTuner, 5);
        result &= fc2580_i2c_read(pTuner, 0x2F, &cal_mon);
        if ((cal_mon & 0xC0) != 0xC0)
        {
            result &= fc2580_i2c_write(pTuner, 0x2E, 0x01);
            result &= fc2580_i2c_write(pTuner, 0x2E, 0x09);
        }
        else
            break;
    }

    result &= fc2580_i2c_write(pTuner, 0x2E, 0x01);

    return result;
}
