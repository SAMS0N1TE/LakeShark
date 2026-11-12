#ifndef FM_SPECTRUM_H
#define FM_SPECTRUM_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FM_SPECTRUM_BINS 256
#define FM_SPECTRUM_PERIOD_MS 30
#define FM_SPECTRUM_FLOOR_DB (-100.0f)
#define FM_SPECTRUM_TOP_DB (0.0f)
typedef struct {
    uint32_t center_hz, span_hz, captured_ms, sequence;
} fm_spectrum_snapshot_t;
void fm_spectrum_enable(bool enabled);
void fm_spectrum_invalidate(void);
void fm_spectrum_feed(const uint8_t *iq, int len, uint32_t center_hz,
                      uint32_t span_hz, uint32_t now_ms);
bool fm_spectrum_read(float *bins, int n, uint32_t now_ms,
                      fm_spectrum_snapshot_t *snapshot);
/* Zoom: 1 (the whole span), 2, 4, 8, 16 or 32 times narrower, centred on
   view_hz. The narrower view is computed from the IQ, not cropped, so its
   bins are that much finer. A view_hz of 0, or one the next block's span
   cannot hold, centres it on the receiver. False for any other factor. */
#define FM_SPECTRUM_ZOOM_MAX 32
bool fm_spectrum_zoom(unsigned factor, uint32_t view_hz);
unsigned fm_spectrum_zoom_factor(void);
#ifdef __cplusplus
}
#endif
#endif
