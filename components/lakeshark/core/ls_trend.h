/* A bounded history of one number, for drawing as a line. */

#ifndef LS_TREND_H
#define LS_TREND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 128 samples. At the two-second interval ls_vitals uses that is a little
   over four minutes, which is longer than any screen is wide - the extra is
   what makes the minimum and maximum worth reading. */
#define LS_TREND_MAX 128

typedef struct {
    uint16_t v[LS_TREND_MAX];
    uint8_t  head;      /* where the next sample goes */
    uint8_t  len;       /* how many are valid, up to LS_TREND_MAX */
} ls_trend_t;

void ls_trend_reset(ls_trend_t *t);
void ls_trend_push(ls_trend_t *t, uint16_t v);

/* How many samples are held, 0..LS_TREND_MAX. */
int ls_trend_len(const ls_trend_t *t);

uint16_t ls_trend_at(const ls_trend_t *t, int back);

/* The extremes over the newest `count` samples, or over everything held when `count` is zero or larger than the ring. */

uint16_t ls_trend_min(const ls_trend_t *t, int count);
uint16_t ls_trend_max(const ls_trend_t *t, int count);

/* A value scaled to the eight heights a trace glyph has. */

int ls_trend_level(uint16_t v, uint16_t lo, uint16_t hi);

#ifdef __cplusplus
}
#endif

#endif /* LS_TREND_H */
