/* Text arithmetic, with nothing drawing behind it. */

#ifndef LS_TEXT_H
#define LS_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Break text onto lines at word boundaries. */

int ls_wrap_text(const char *text, int width, char *dst, size_t stride,
                 int max_lines);

/* Seconds of age as an idle screen shows them: exact for the first ten, then
   in steps of ten, so a counter beside data that stopped does not redraw the
   screen every second (nothing moves but when its data does). */
static inline long ls_age_q(long s)
{
    return s < 10 ? (s < 0 ? 0 : s) : s - s % 10;
}

/* How long ago, in four columns. */

void ls_age_str(char *out, size_t cap, uint32_t then, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif /* LS_TEXT_H */
