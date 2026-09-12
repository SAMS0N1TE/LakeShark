#ifndef CARTO_LABEL_H
#define CARTO_LABEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
