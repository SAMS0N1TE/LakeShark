#ifndef REC_LR_IQ_H
#define REC_LR_IQ_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REC_LR_IQ_SAMPLES 32767u
#define REC_LR_IQ_RATE 125000u
#define REC_LR_IQ_TX_RATE 19200u
#define REC_LR_IQ_BYTES (REC_LR_IQ_SAMPLES * 4u)

/* LakeShark SubGhz IQ, version 1: IQ_Data contains IQ16LE hex. */
bool rec_lr_iq_save(const char *path, uint32_t hz, const uint8_t *iq, size_t bytes);
bool rec_lr_iq_load(const char *path, uint32_t *hz, uint8_t *iq, size_t cap, size_t *bytes);
bool rec_lr_iq_rate_supported(uint32_t rate);
/* In-place safe; rate selects OOK output. Non-OOK uses
   125000 to preserve hardware deviation; report.rate is the actual rate. */
size_t rec_lr_iq_convert(const uint8_t *iq, size_t bytes, uint8_t *out, size_t cap, uint32_t rate);
/* Optional output identifies regenerated OOK versus gated, filtered IQ. */
size_t rec_lr_iq_convert_path(const uint8_t *iq, size_t bytes, uint8_t *out, size_t cap, uint32_t rate, bool *clean_ook);
typedef struct {
    bool ook;
    double bimodal, clustered, phase;
    double tone_separation_hz, tone_share[2]; /* Low/high frequency sample shares. */
    double bandwidth_hz;
    uint32_t rate;
    unsigned pulses;
    unsigned mid_burst_switches; /* Tone changes outside envelope edge guards. */
} rec_lr_iq_classifier_t;
size_t rec_lr_iq_convert_report(const uint8_t *iq, size_t bytes, uint8_t *out, size_t cap,
                               uint32_t rate, rec_lr_iq_classifier_t *report);
/* Callback receives complete newline-terminated records; false cancels output. */
typedef bool (*rec_lr_iq_dump_emit_t)(void *ctx, const char *line);
bool rec_lr_iq_dump(const uint8_t *iq, size_t bytes, uint32_t hz,
                    rec_lr_iq_dump_emit_t emit, void *ctx);
/* Five complete PWM frames, I=48/Q=0, rounded absolute edge times. */
size_t rec_lr_iq_test(uint8_t *out, size_t cap, uint32_t rate);
/* 4800 baud, +/-20 kHz, 32-bit 1010 preamble + A53C96 + 1010, five frames. */
size_t rec_lr_iq_test_fsk(uint8_t *out, size_t cap, uint32_t rate);
typedef struct {
    size_t samples, above;
    double peak, median;
    unsigned pulses, short_us, long_us;
    char bits[25]; /* First slicer frame, 400/1200 us PWM; '?' if unmatched. */
} rec_lr_iq_stats_t;
/* Euclidean |IQ| in raw IQ16 units. Includes EOF-flushed slicer runs. */
bool rec_lr_iq_stats(const uint8_t *iq, size_t bytes, rec_lr_iq_stats_t *out);
#endif
