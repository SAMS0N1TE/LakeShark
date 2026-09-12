#ifndef CARTO_LABEL_H
#define CARTO_LABEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*LS-1055  Place names, handed out rather than drawn.

   The renderer draws a place as a dot and always did, because a label at map
   resolution means a bitmap font and somewhere to put it. On a character
   grid the answer is different: the name is TEXT, drawn in cells over the
   picture, which is the one thing a character grid is better at than a
   framebuffer.

   So the parser collects names instead of rendering them. It already walks
   the place layer and already reads one tag value - the class - so a name is
   the same walk with another key.

   `min_zoom` and `rank` come along because the tiles carry them and because
   without them every name arrives at once: a country, a state, a city and a
   hamlet all have a point in the same layer, and a screen that draws them
   all is unreadable at every scale. They are what lets a caller decide which
   names this view is big enough for.

   This is a public header because the sink is the caller's memory and the
   caller has to declare one. */

#define CARTO_LABEL_MAX_TEXT 32

typedef struct {
    char    text[CARTO_LABEL_MAX_TEXT];
    int     x, y;        /* pixels in the frame just rendered */
    uint8_t min_zoom;    /* 0 when the tile did not say */
    uint8_t rank;        /* population_rank; bigger is more, 0 when absent */
} carto_label;

typedef struct {
    carto_label *at;
    int          cap;
    int          n;      /* the caller zeroes this before a frame */
} carto_label_sink;

#ifdef __cplusplus
}
#endif

#endif
