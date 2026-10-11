#ifndef P25_LR_IQ_H
#define P25_LR_IQ_H
#include <stddef.h>
#include <stdint.h>
/* One independent IQ16LE window at 125 ksample/s -> U8 IQ at 240 ksample/s.
   Linear interpolation, peak normalization; no state crosses capture gaps.
   Returns output bytes, or zero for invalid input / insufficient capacity. */
size_t p25_lr_iq_convert(const uint8_t *iq, size_t bytes, uint8_t *out, size_t capacity);
#endif
