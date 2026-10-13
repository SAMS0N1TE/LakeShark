#ifndef REC_LR_OOK_H
#define REC_LR_OOK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* On/off keying from the LR2021's raw I/Q stream: 125 kS/s, 16-bit I and Q
   little endian, 8 us a sample. The same job the RTL slicer does in REC,
   sized for this stream, so READ on the LR2021 hears any OOK remote rather
   than only an FSK packet with a known sync word.

   Fixed storage, no allocation, no blocking: it runs inside the stream
   owner's consumer callback. */

#define REC_LR_OOK_US_PER_SAMPLE 8u
#define REC_LR_OOK_MAX_EDGES     2048

typedef struct {
    uint32_t min_pulse_us;   /* shorter runs are folded into the neighbour */
    uint32_t gap_us;         /* this much quiet ends a capture             */
    int      min_edges;      /* fewer than this is a blip, not a capture   */
} rec_lr_ook_cfg_t;

typedef struct {
    rec_lr_ook_cfg_t cfg;
    int32_t  floor_acc;      /* noise floor, Q6                            */
    int32_t  floor;
    int32_t  peak;           /* strongest magnitude in the capture         */
    bool     carrier, level, capturing;
    uint32_t run;            /* samples at the current level               */
    int      edges;
    uint32_t span_us;
    int32_t  edge[REC_LR_OOK_MAX_EDGES];
} rec_lr_ook_t;

/* A finished capture: edges in microseconds, positive high, negative low. */
typedef void (*rec_lr_ook_emit_t)(void *ctx, const int32_t *edge, int edges, int peak);

void rec_lr_ook_init(rec_lr_ook_t *s, const rec_lr_ook_cfg_t *cfg);
/* Drop a capture in progress: the stream had a gap. */
void rec_lr_ook_reset(rec_lr_ook_t *s);
void rec_lr_ook_feed(rec_lr_ook_t *s, const uint8_t *iq16le, size_t bytes,
                     rec_lr_ook_emit_t emit, void *ctx);
#endif
