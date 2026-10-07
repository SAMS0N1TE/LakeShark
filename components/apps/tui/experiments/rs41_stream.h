#ifndef RS41_STREAM_H
#define RS41_STREAM_H
#include "rs41_decode.h"
typedef struct {
    uint64_t sync;
    uint8_t wire[518], byte;
    unsigned used, bits;
    bool collecting;
} rs41_stream_t;
typedef void (*rs41_frame_fn)(const rs41_report_t *, void *);
/* Restart after a hardware sync; inverted normalizes receiver polarity. */
void rs41_stream_reset(rs41_stream_t *s, bool after_sync);
void rs41_stream_feed(rs41_stream_t *s, rs41_decoder_t *d, const uint8_t *msb,
                      size_t n, bool inverted, rs41_frame_fn fn, void *arg);
#endif
