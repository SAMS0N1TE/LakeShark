#pragma once
#include "e3d.h"
#include <stddef.h>

#define BSP30_FILE_LIMIT (8 * 1024 * 1024)
#define BSP30_E3D_SCALE (1.0f / 16)

typedef struct bsp30_scene bsp30_scene;

typedef struct {
    int leaf, faces, triangles, textured_faces, lightmapped_faces, chunks, visible_faces, visible_chunks;
} bsp30_stats;

/* The scene borrows the supplied BSP data until close. A brush mesh borrows
 * map textures from the scene and must be freed before the scene is closed. */
bsp30_scene *bsp30_open(const void *data, size_t length);
void bsp30_close(bsp30_scene *scene);
const uint8_t *bsp30_entities(const bsp30_scene *scene, size_t *length);

/* Points and eye coordinates are in BSP units. Mesh vertices use e3d axes and scale. */
bool bsp30_point_open(bsp30_scene *scene, const float point[3]);
void bsp30_prepare_world(bsp30_scene *scene, const float eye[3]);
/* Replace the optional opaque texture cache for the prepared leaf. Call between
 * frames. Zero bytes releases it. Returns the number of bytes populated. */
size_t bsp30_cache_visible_lightmaps(bsp30_scene *scene, size_t byte_budget);
void bsp30_draw_world(bsp30_scene *scene, e3d_t *engine);
e3d_mesh *bsp30_brush_mesh(bsp30_scene *scene, unsigned model, bool omit_glass);
void bsp30_mesh_free(e3d_mesh *mesh);
bsp30_stats bsp30_get_stats(const bsp30_scene *scene);
