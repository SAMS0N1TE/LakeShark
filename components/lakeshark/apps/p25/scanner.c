
#include "scanner.h"
#include "p25_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void scanner_init(void)
{
    memset(&SCAN, 0, sizeof(SCAN));
    SCAN.start_freq  = 150000000UL;
    SCAN.stop_freq   = 156000000UL;
    SCAN.step_hz     = 100000;
    SCAN.num_bins    = (SCAN.stop_freq - SCAN.start_freq) / SCAN.step_hz;
    if (SCAN.num_bins > SCAN_BINS_MAX) SCAN.num_bins = SCAN_BINS_MAX;
    SCAN.noise_floor = -35.0f;
    for (int r = 0; r < SCAN_WATERFALL_ROWS; r++)
        for (int b = 0; b < SCAN_BINS_MAX; b++)
            SCAN.waterfall[r][b] = -40.0f;
}

static bool scanner_read_block(ls_radio_session_t *session, uint8_t *iq,
                               size_t bytes,
                               const volatile bool *keep_running)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < bytes && timeouts < 10 && *keep_running) {
        size_t part = 0;
        ls_radio_err_t error = ls_radio_iq_read(session, iq + got,
                                                bytes - got, 20, &part);
        if (error == LS_RADIO_OK) {
            got += part;
            continue;
        }
        if (error != LS_RADIO_ERR_TIMEOUT) return false;
        ++timeouts;
    }
    return got == bytes;
}

uint32_t scanner_run_sweep(ls_radio_session_t *session,
                           const volatile bool *keep_running)
{
    if (!session || !keep_running) return 0;
    SCAN.scanning = true;
    sys_log(2, "Sweep %.3f-%.3f MHz  %d bins",
            SCAN.start_freq / 1e6, SCAN.stop_freq / 1e6, SCAN.num_bins);

    uint8_t *iq = heap_caps_malloc(SCAN_IQ_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    if (!iq) iq = malloc(SCAN_IQ_SAMPLES * 2);
    if (!iq) {
        sys_log(4, "Sweep OOM");
        SCAN.scanning = false;
        return s_tune_freq_hz;
    }

    float peak_pwr = -999.0f;
    int peak_bin = 0;
    float sum_pwr = 0;
    int valid_bins = 0;

    for (int b = 0; b < SCAN.num_bins && *keep_running; b++) {
        uint32_t freq = SCAN.start_freq + (uint32_t)b * SCAN.step_hz + SCAN.step_hz / 2;

        uint64_t actual_hz = 0;
        ls_radio_err_t tune_error = ls_radio_iq_retune(session, freq, true,
                                                        &actual_hz);
        if (tune_error != LS_RADIO_OK) {
            SCAN.power[b] = -99.0f;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5));

        /* LS-180: Preserve the original settle/read-discard/read-measure
         * policy, but keep the asynchronous session running. The adapter's
         * fast-retune contract, rather than this scanner, owns stale transport
         * data and FIFO handling. */
        if (!scanner_read_block(session, iq, SCAN_IQ_SAMPLES * 2,
                                keep_running) ||
            !scanner_read_block(session, iq, SCAN_IQ_SAMPLES * 2,
                                keep_running)) {
            SCAN.power[b] = -99.0f;
            continue;
        }

        double pwr = 0;
        int ns = SCAN_IQ_SAMPLES;
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

    if (!*keep_running) {
        heap_caps_free(iq);
        SCAN.scanning = false;
        return 0;
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

    uint32_t restored_hz = 0;
    if (*keep_running) {
        (void)ls_radio_iq_retune(session, s_tune_freq_hz, false,
                                 &restored_hz);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    heap_caps_free(iq);
    SCAN.scanning = false;
    return restored_hz;
}
