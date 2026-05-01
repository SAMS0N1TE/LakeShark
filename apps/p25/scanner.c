/*
 * scanner.c — Spectrum sweep engine
 */
#include "scanner.h"
#include "p25_state.h"
#include "usb/usb_host.h"
#include "rtl-sdr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

void scanner_init(void)
{
    memset(&SCAN, 0, sizeof(SCAN));
    SCAN.start_freq  = 150000000UL;
    SCAN.stop_freq   = 156000000UL;   /* R820T2 PLL fix: full range works now */
    SCAN.step_hz     = 100000;
    SCAN.num_bins    = (SCAN.stop_freq - SCAN.start_freq) / SCAN.step_hz;
    if (SCAN.num_bins > SCAN_BINS_MAX) SCAN.num_bins = SCAN_BINS_MAX;
    SCAN.noise_floor = -35.0f;
    for (int r = 0; r < SCAN_WATERFALL_ROWS; r++)
        for (int b = 0; b < SCAN_BINS_MAX; b++)
            SCAN.waterfall[r][b] = -40.0f;
}

void scanner_run_sweep(void)
{
    if (!rtldev) return;
    SCAN.scanning = true;
    sys_log(2, "Sweep %.3f-%.3f MHz  %d bins",
            SCAN.start_freq / 1e6, SCAN.stop_freq / 1e6, SCAN.num_bins);

    uint8_t *iq = malloc(SCAN_IQ_SAMPLES * 2);
    if (!iq) { sys_log(4, "Sweep OOM"); SCAN.scanning = false; return; }

    float peak_pwr = -999.0f;
    int peak_bin = 0;
    float sum_pwr = 0;
    int valid_bins = 0;

    for (int b = 0; b < SCAN.num_bins; b++) {
        uint32_t freq = SCAN.start_freq + (uint32_t)b * SCAN.step_hz + SCAN.step_hz / 2;

        int sr = rtlsdr_set_center_freq(rtldev, freq);
        if (sr < 0) {
            /* PLL failed — mark this bin as invalid, don't count it */
            SCAN.power[b] = -99.0f;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5));

        int nr = 0;
        rtlsdr_read_sync(rtldev, iq, SCAN_IQ_SAMPLES * 2, &nr);  /* flush */
        nr = 0;
        int r = rtlsdr_read_sync(rtldev, iq, SCAN_IQ_SAMPLES * 2, &nr);
        if (r < 0 || nr < (int)(SCAN_IQ_SAMPLES * 2)) {
            SCAN.power[b] = -99.0f;
            continue;
        }

        double pwr = 0;
        int ns = nr / 2;
        for (int i = 0; i < ns; i++) {
            float fi = ((float)iq[i*2]   - 127.5f) / 127.5f;
            float fq = ((float)iq[i*2+1] - 127.5f) / 127.5f;
            pwr += (double)(fi*fi + fq*fq);
        }
        float db = 10.0f * log10f((float)(pwr / ns) + 1e-12f);
        SCAN.power[b] = db;
        sum_pwr += db;
        valid_bins++;
        if (db > peak_pwr) { peak_pwr = db; peak_bin = b; }
        vTaskDelay(1);
    }

    SCAN.peak_bin   = peak_bin;
    SCAN.peak_power = peak_pwr;
    SCAN.peak_freq  = SCAN.start_freq + (uint32_t)peak_bin * SCAN.step_hz + SCAN.step_hz / 2;
    SCAN.noise_floor = valid_bins > 0 ? sum_pwr / valid_bins : -35.0f;

    memcpy(SCAN.waterfall[SCAN.wf_row], SCAN.power, sizeof(float) * SCAN.num_bins);
    SCAN.wf_row = (SCAN.wf_row + 1) % SCAN_WATERFALL_ROWS;
    SCAN.sweep_count++;

    sys_log(1, "Peak=%.4f MHz  %.1f dB  SNR=%.1f  (%d/%d bins ok)",
            SCAN.peak_freq / 1e6, SCAN.peak_power, SCAN.peak_power - SCAN.noise_floor,
            valid_bins, SCAN.num_bins);

    rtlsdr_set_center_freq(rtldev, s_tune_freq_hz);
    rtlsdr_reset_buffer(rtldev);
    vTaskDelay(pdMS_TO_TICKS(10));

    free(iq);
    SCAN.scanning = false;
}
