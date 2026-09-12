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

/* How long ago, in four columns. */

void ls_age_str(char *out, size_t cap, uint32_t then, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif /* LS_TEXT_H */
