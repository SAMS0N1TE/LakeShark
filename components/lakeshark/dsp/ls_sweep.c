/*LS-820  Radio side of the sweep. See ls_sweep.h. */

#include "ls_sweep.h"
#include "spectrum.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ls_sweep";

/* One transform's worth of interleaved u8 IQ. */
#define IQ_BYTES_PER_FFT (SPEC_FFT_N * 2)

/* Long enough that a stalled dongle is reported rather than hanging the
   console, short enough that a whole sweep cannot sit here for minutes. */
#define READ_TIMEOUT_MS 500

/*LS-821  Post-retune settle. See the comment at the read loop: too little and
   the sweep transforms the tuner's own slide between frequencies. */
#define LS_SWEEP_SETTLE_MS        4
#define LS_SWEEP_DISCARD_BUFFERS  2

ls_radio_err_t ls_sweep_run(const ls_sweep_plan_t *plan, int gain_tenths,
                            uint32_t dwell_ffts, bool fast_retune, int8_t *dbfs,
                            ls_sweep_progress_fn progress, void *user)
{
    if (!plan || !dbfs || plan->n_bins == 0) return LS_RADIO_ERR_INVALID;
    if (dwell_ffts == 0) dwell_ffts = LS_SWEEP_DEFAULT_DWELL_FFTS;

    const ls_radio_requirements_t requirements = {
        .required_caps   = LS_RADIO_RX_IQ_U8,
        .min_hz          = plan->start_hz > plan->half_span_hz
                               ? plan->start_hz - plan->half_span_hz : 0,
        .max_hz          = plan->stop_hz + plan->half_span_hz,
        .sample_rate_hz  = plan->sample_rate_hz,
        .iq_format       = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
    };

    ls_radio_session_t *session = NULL;
    ls_radio_err_t error = ls_radio_acquire("sweep", &requirements, &session);
    if (error != LS_RADIO_OK) return error;

    /* Both buffers are touched linearly by this task alone - no DMA, no ISR -
       so they belong in PSRAM rather than the internal heap this project
       keeps running out of. */
    size_t   iq_bytes = (size_t)dwell_ffts * IQ_BYTES_PER_FFT;
    uint8_t *iq = heap_caps_malloc(iq_bytes, MALLOC_CAP_SPIRAM);
    float   *db = heap_caps_malloc(sizeof(float) * SPEC_FFT_N, MALLOC_CAP_SPIRAM);
    if (!iq) iq = malloc(iq_bytes);
    if (!db) db = malloc(sizeof(float) * SPEC_FFT_N);
    if (!iq || !db) {
        free(iq); free(db);
        ls_radio_release(session);
        return LS_RADIO_ERR_NO_MEMORY;
    }

    spectrum_init();
    ls_sweep_reset(plan, dbfs);

    const ls_radio_iq_config_t requested = {
        .center_hz      = ls_sweep_tune_center(plan, 0),
        .sample_rate_hz = plan->sample_rate_hz,
        .bandwidth_hz   = 0,
        .gain_mode      = gain_tenths == 0 ? LS_RADIO_GAIN_AUTO
                                           : LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = gain_tenths,
    };
    ls_radio_iq_config_t actual;
    error = ls_radio_iq_configure(session, &requested, &actual);

    /*LS-822  The plan's bin arithmetic is derived from the sample rate, so a
       radio that quietly gave us a different one would place every peak at the
       wrong frequency - by the ratio of the two rates, uniformly, which looks
       exactly like a plausible spectrum of somewhere else. Refuse rather than
       report a confident wrong answer. */
    if (error == LS_RADIO_OK && actual.sample_rate_hz != plan->sample_rate_hz) {
        ESP_LOGE(TAG, "planned for %u S/s but the radio gave %u - refusing to "
                      "map bins with the wrong rate",
                 (unsigned)plan->sample_rate_hz,
                 (unsigned)actual.sample_rate_hz);
        error = LS_RADIO_ERR_UNSUPPORTED;
    }

    if (error == LS_RADIO_OK) error = ls_radio_iq_start(session);

    for (uint32_t t = 0; error == LS_RADIO_OK && t < plan->n_tunes; t++) {
        uint64_t center = ls_sweep_tune_center(plan, t);
        uint64_t tuned  = 0;

        /* fast=false so the driver resets the transfer buffer: samples already
           in flight were taken at the previous centre, and folding them in
           would put a signal at a frequency it was never on. */
        if (t > 0) {
            error = ls_radio_iq_retune(session, center, fast_retune, &tuned);
            if (error != LS_RADIO_OK) break;
        }

        spectrum_reset();

        /*LS-821  Let the tuner actually land before believing anything.

           This discarded one 512-sample buffer - 213 microseconds - which is
           nothing next to an R820T2 PLL lock. Transforming samples taken while
           the tuner is still sliding between frequencies folds a chirp across
           the whole band into every bin, and the result is a flat raised floor
           with no sharp peaks. Measured on the LCD 4.3 sweeping 88-108 MHz:
           a 12-18 dB spread at every gain setting, with the strongest bins
           moving between runs - noise dressed as spectrum.

           Sleep first, then throw away whole buffers. At 24 tunes the extra
           cost is a fraction of a second against a sweep that was previously
           measuring nothing. */
        vTaskDelay(pdMS_TO_TICKS(LS_SWEEP_SETTLE_MS));

        size_t got = 0;
        for (int d = 0; d < LS_SWEEP_DISCARD_BUFFERS; d++) {
            if (ls_radio_iq_read(session, iq, iq_bytes, READ_TIMEOUT_MS,
                                 &got) != LS_RADIO_OK) break;
        }

        uint32_t done = 0;
        while (done < dwell_ffts) {
            got = 0;
            error = ls_radio_iq_read(session, iq, iq_bytes, READ_TIMEOUT_MS,
                                     &got);
            if (error != LS_RADIO_OK) break;

            /* spectrum_accum transforms the FIRST 512 complex samples of what
               it is handed and ignores the rest, so walk the buffer rather
               than handing it the whole thing and averaging one transform
               eight times. */
            size_t whole = got / IQ_BYTES_PER_FFT;
            for (size_t i = 0; i < whole && done < dwell_ffts; i++, done++)
                spectrum_accum(iq + i * IQ_BYTES_PER_FFT, IQ_BYTES_PER_FFT);

            if (whole == 0) break;   /* short read; take what we have */
        }
        if (error != LS_RADIO_OK) break;

        if (spectrum_read_db(db, SPEC_FFT_N))
            ls_sweep_fold(plan, center, db, SPEC_FFT_N, dbfs);

        if (progress) progress(t + 1, plan->n_tunes, user);

        /* The console task must not starve the rest of the system for the
           length of a sweep. */
        if ((t & 0x0f) == 0x0f) vTaskDelay(1);
    }

    ls_radio_iq_stop(session);
    ls_radio_release(session);
    free(iq);
    free(db);

    if (error != LS_RADIO_OK)
        ESP_LOGW(TAG, "sweep stopped: %s", ls_radio_err_name(error));
    return error;
}
