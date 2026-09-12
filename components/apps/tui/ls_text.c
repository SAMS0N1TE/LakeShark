

#include "ls_text.h"

#include <stdio.h>
#include <string.h>

int ls_wrap_text(const char *text, int width, char *dst, size_t stride,
                 int max_lines)
{
    if (!text || !dst || width <= 4 || max_lines <= 0 || stride < 2) return 0;

    const int len = (int)strlen(text);
    int off = 0, n = 0;

    while (off < len && n < max_lines) {
        int take = len - off;
        if (take > width) {
            take = width;
            /* Back up to a space when there is one, so a word is not cut in
               half by the edge of the panel. */
            int b = take;
            while (b > width / 2 && text[off + b] != ' ') b--;
            if (b > width / 2) take = b;
        }
        snprintf(dst + (size_t)n * stride, stride, "%.*s", take, text + off);
        n++;
        off += take;
        while (off < len && text[off] == ' ') off++;
    }
    return n;
}

/* See ls_text.h. The widths are fixed at two digits so a column of
   these stays a column; a duration long enough to need three has already
   made the point it was there to make. */
void ls_age_str(char *out, size_t cap, uint32_t then, uint32_t now)
{
    if (!out || cap < 2) return;
    if (!then || now < then) { snprintf(out, cap, " -- "); return; }

    const uint32_t d = now - then;
    if (d < 60)          snprintf(out, cap, "%2lus", (unsigned long)d);
    else if (d < 3600)   snprintf(out, cap, "%2lum", (unsigned long)(d / 60));
    else if (d < 86400)  snprintf(out, cap, "%2luh", (unsigned long)(d / 3600));
    else                 snprintf(out, cap, "%2lud", (unsigned long)(d / 86400));
}
