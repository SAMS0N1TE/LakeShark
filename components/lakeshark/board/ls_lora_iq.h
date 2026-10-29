/* Pure LR2021 ROM 1.24 I/Q routing and sample helpers. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LR20XX_IQ16, LR20XX_IQ8 } lr20xx_iq_format_t;
typedef enum {
    LR20XX_IQ_SOURCE2, LR20XX_IQ_SOURCE0_576, LR20XX_IQ_SOURCE1
} lr20xx_iq_route_t;

uint32_t lr20xx_iq_source1_rate(uint8_t code);
bool lr20xx_iq_raw_route(uint8_t code, uint32_t rate, unsigned source, uint8_t out[11]);
bool lr20xx_iq_route_build(lr20xx_iq_route_t kind, uint8_t code, uint32_t source2_rate,
                          uint8_t out[11], uint32_t *rate, lr20xx_iq_format_t *format);
/* Non-overlapping buffers; pairs is a count of signed I,Q pairs. */
void lr20xx_iq_expand8(const uint8_t *in, int16_t *out, size_t pairs);

#ifdef __cplusplus
}
#endif
