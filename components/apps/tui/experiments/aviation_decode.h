#ifndef AVIATION_DECODE_H
#define AVIATION_DECODE_H
#include <stddef.h>
#include <stdint.h>
typedef enum { AV_UNKNOWN, AV_A, AV_C, AV_S, AV_AC, AV_X, AV_Y } av_class_t;
/* MSB-first 2 Mchip/s samples, including the triggering pulse. */
av_class_t aviation_classify(const uint8_t *chips, size_t bytes, int band);
/* Mode S payload chips after the hardware's 16-chip preamble. -1 on bad PPM. */
int aviation_df(const uint8_t *chips, size_t bytes);
#endif
