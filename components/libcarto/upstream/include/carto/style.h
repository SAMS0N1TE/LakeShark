#ifndef CARTO_STYLE_H
#define CARTO_STYLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint8_t r, g, b; } carto_rgb;

#define CARTO_TILE_EXTENT 4096
#define CARTO_TILE_SIZE   256

typedef enum {
    CARTO_LAYER_WATER    = 0,
    CARTO_LAYER_LANDUSE  = 1,
    CARTO_LAYER_BUILDING = 2,
    CARTO_LAYER_ROAD     = 3,
    CARTO_LAYER_PLACE    = 4,
    CARTO_LAYER_BOUNDARY = 5,   /* LS DEVIATION 5: borders, as lines */
    CARTO_LAYER_COUNT    = 6
} carto_layer_kind;

#define CARTO_ROAD_PRIO_MIN 1
#define CARTO_ROAD_PRIO_MAX 10

typedef struct {
    carto_rgb bg;
    carto_rgb water;
    carto_rgb park;
    carto_rgb building;

    carto_rgb road_color;
    uint8_t   road_width[CARTO_ROAD_PRIO_MAX + 1];
    carto_rgb road_color_by_prio[CARTO_ROAD_PRIO_MAX + 1];

    carto_rgb label_color;
    carto_rgb halo_color;

    carto_rgb aircraft_color;
    carto_rgb aircraft_selected_color;
    carto_rgb aircraft_emergency_color;
    carto_rgb aircraft_label_color;
    carto_rgb aircraft_halo_color;

    bool draw_labels;
    carto_rgb boundary;         /* LS DEVIATION 5 */
    bool water_outline;         /* LS DEVIATION 5: shores as lines, no fill */
} carto_style;

#define CARTO_MIN_LINE_PX 3.0f
#define CARTO_MIN_POLY_PX 2.0f

void carto_style_default(carto_style *out);
int carto_road_priority(const char *cls);
/* LS DEVIATION 5: the finer class (kind_detail); 0 for one it does not know. */
int carto_road_detail_priority(const char *detail);
/* Country 3, region 2, county 1, anything else 0 (not drawn). */
int carto_boundary_rank(const char *kind);
carto_layer_kind carto_classify_layer(const char *layer_name);

#ifdef __cplusplus
}
#endif

#endif
