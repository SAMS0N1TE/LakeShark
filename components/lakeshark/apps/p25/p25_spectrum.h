#ifndef P25_SPECTRUM_H
#define P25_SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LS-692: SIGNAL is a view of IQ already owned by the P25 receive task.  It is
 * never a scan request and never acquires or retunes a radio.  One 512-point
 * FFT per eight 16 KiB receive blocks bounds the added work to about 3.7 FFT/s
 * at 240 kSPS.  The publication arrays live in PSRAM in p25_spectrum.c. */
#define P25_SPECTRUM_BINS             512
#define P25_SPECTRUM_BLOCK_STRIDE       8
#define P25_SPECTRUM_FLOOR_DB         (-85.0f)
#define P25_SPECTRUM_TOP_DB           (-20.0f)
#define P25_SPECTRUM_STALE_MS          1200u

typedef enum {
    P25_SPECTRUM_OWNER_NONE = 0,
    P25_SPECTRUM_OWNER_RX   = 1,
} p25_spectrum_owner_t;

typedef enum {
    P25_SPECTRUM_FEED_DISABLED = 0,
    P25_SPECTRUM_FEED_NOT_OWNER,
    P25_SPECTRUM_FEED_INVALID,
    P25_SPECTRUM_FEED_DEFERRED,
    P25_SPECTRUM_FEED_PUBLISHED,
} p25_spectrum_feed_result_t;

typedef struct {
    uint32_t center_hz;       /* actual radio centre, not requested setting */
    uint32_t span_hz;         /* actual IQ sample rate                     */
    uint32_t filter_hz;       /* actual tuner bandwidth; zero means AUTO   */
    uint32_t captured_ms;
    uint32_t sequence;
    uint16_t fft_bins;
    uint8_t  blocks_per_fft;
    bool     following_voice;
} p25_spectrum_snapshot_t;

/* Initialise before the RX task starts. Reinitialising is supported by the
 * host fixture and invalidates every earlier snapshot. */
void p25_spectrum_init(void);

/* The UI enables production only while SIGNAL is visible. Changing the state
 * invalidates the old generation, so reopening SIGNAL cannot display an old
 * carrier as a fresh one. */
void p25_spectrum_enable(bool enabled);
bool p25_spectrum_enabled(void);
void p25_spectrum_invalidate(void);

/* Called only by the task that owns the P25 IQ session. `span_hz` and
 * `filter_hz` are the actual values returned by ls_radio_iq_configure().
 * Traffic IQ is intentionally accepted: observing the owned stream does not
 * start the second tuner sweep that would interrupt grant following. */
p25_spectrum_feed_result_t p25_spectrum_feed_iq(
    p25_spectrum_owner_t owner, const uint8_t *iq, int len,
    uint32_t center_hz, uint32_t span_hz, uint32_t filter_hz,
    bool following_voice, uint32_t now_ms);

/* Copy a coherent, MAX-resampled 0..1 frame. False means disabled, never
 * published in this generation, concurrently changing, or older than
 * max_age_ms. A stale frame is left in storage so the UI can leave its last
 * drawing up, but it is never returned as tunable evidence. */
bool p25_spectrum_read(float *out, int n, uint32_t now_ms,
                       uint32_t max_age_ms,
                       p25_spectrum_snapshot_t *snapshot);

/* Map a pixel in the displayed passband to an absolute frequency. Both the
 * coordinate and resulting tuner frequency are checked; out-of-panel taps do
 * not silently clamp to a different channel. */
bool p25_spectrum_tap_hz(int x, int width, uint32_t center_hz,
                         uint32_t span_hz, uint32_t tuner_min_hz,
                         uint32_t tuner_max_hz, uint32_t *out_hz);

#ifdef __cplusplus
}
#endif

#endif
