#ifndef LR_CHIP_WF_H
#define LR_CHIP_WF_H
#include "esp_err.h"
#include "lr_iq_spectrum.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The LR waterfall computed by the radio itself (lr20xx_chip_spectrum): the
 * LR2021 captures, runs the FFT and converts to dB on its own CPU, and this
 * session only averages its frames into 100 ms rows. The ESP32-P4 does no
 * FFT and reads 256 bytes a frame instead of the I/Q, so no capture task
 * polls the FIFO. Rows span LR_CHIP_WF_SPAN_HZ around the tune.
 * Same ownership rules as lr_wf_session: the UI holds the radio until stop
 * returns; start is nonblocking and restarts on a new frequency. */
#define LR_CHIP_WF_SPAN_HZ 250000u
esp_err_t lr_chip_wf_start(uint32_t hz);
void lr_chip_wf_stop(void);
/* Bins 0..1 over -100..0 dBFS, low frequency first; false until a fresh row. */
bool lr_chip_wf_read(float *bins, lr_spec_line *line, uint32_t now_ms);
/* The session ended on its own (the chip refused the program, or a reset):
   the caller falls back to the I/Q waterfall. Cleared by stop. */
bool lr_chip_wf_failed(void);
typedef struct {
    bool active; uint32_t hz, rows, frames, frames_per_row;
    esp_err_t error;
} lr_chip_wf_status_t;
void lr_chip_wf_status(lr_chip_wf_status_t *out);
#ifdef __cplusplus
}
#endif
#endif
