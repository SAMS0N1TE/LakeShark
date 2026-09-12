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
#ifdef __cplusplus
}
#endif
#endif
