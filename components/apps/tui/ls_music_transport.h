#ifndef LS_MUSIC_TRANSPORT_H
#define LS_MUSIC_TRANSPORT_H

#include <stdbool.h>

static inline int ls_music_step(int selected, int count, int delta) {
    if (count <= 0) return -1;
    if (selected < 0 || selected >= count) selected = 0;
    if (delta > 0) return selected == count - 1 ? 0 : selected + 1;
    if (delta < 0) return selected == 0 ? count - 1 : selected - 1;
    return selected;
}

static inline int ls_music_scroll(int selected, int count, int rows, int top) {
    if (count <= 0 || rows <= 0) return 0;
    if (selected < 0 || selected >= count) selected = 0;
    int max_top = count > rows ? count - rows : 0;
    if (top < 0) top = 0;
    if (top > max_top) top = max_top;
    if (selected < top) top = selected;
    else if (selected - top >= rows) top = selected - rows + 1;
    if (top > max_top) top = max_top;
    return top;
}

static inline int ls_music_next(int current, int count, bool repeat_one, bool repeat_all) {
    if (count <= 0 || current < 0 || current >= count) return -1;
    if (repeat_one) return current;
    if (current < count - 1) return current + 1;
    return repeat_all ? 0 : -1;
}

#endif
