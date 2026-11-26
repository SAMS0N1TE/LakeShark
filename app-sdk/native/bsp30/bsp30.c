#include "bsp30.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    ENTITIES, PLANES, TEXTURES, VERTICES, VISIBILITY, NODES, TEXINFO, FACES,
    LIGHTING, CLIPNODES, LEAVES, MARKS, EDGES, SURFEDGES, MODELS, LUMP_COUNT
};
enum { FACES_PER_CHUNK = 32, MAX_CHUNKS = 256, FACE_LIMIT = FACES_PER_CHUNK * MAX_CHUNKS };
enum { MAX_TEXTURES = 255 };
static const float scale = BSP30_E3D_SCALE;

typedef struct {
    const uint8_t *data;
    size_t len;
} span;

struct bsp30_scene {
    span lumps[LUMP_COUNT];
    e3d_mesh *chunks[MAX_CHUNKS];
    e3d_mesh draw_chunks[MAX_CHUNKS];
    e3d_texture textures[MAX_TEXTURES];
    uint16_t (*palettes)[256];
    e3d_lightmap *face_lightmaps;
    uint16_t *lit_cache;
    int texture_count, textured_surfaces, lightmapped_surfaces;
    uint16_t face_triangle_end[FACE_LIMIT];
    int world_root, world_leaves, face_first, face_count, chunk_count;
    int triangles, leaf, visible_chunks, visible_faces;
    uint8_t visible[(FACE_LIMIT + 7) / 8];
};

static uint16_t u16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t u32(const uint8_t *p)
{
    return u16(p) | (uint32_t)u16(p + 2) << 16;
}

static float f32(const uint8_t *p)
{
    uint32_t bits = u32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static const uint8_t *record(bsp30_scene *scene, int lump, int index, unsigned stride)
{
    return index >= 0 && (size_t)index < scene->lumps[lump].len / stride ?
           scene->lumps[lump].data + (size_t)index * stride : NULL;
}

static bool directory(bsp30_scene *scene, span file)
{
    if (file.len < 4 + 8 * LUMP_COUNT || u32(file.data) != 30) return false;
    for (unsigned i = 0; i < LUMP_COUNT; ++i) {
        size_t start = u32(file.data + 4 + i * 8), len = u32(file.data + 8 + i * 8);
        if (start > file.len || len > file.len - start) return false;
        scene->lumps[i] = (span){ file.data + start, len };
    }
    const uint8_t *world = record(scene, MODELS, 0, 64);
    if (!world) return false;
    scene->world_root = (int32_t)u32(world + 36);
    scene->world_leaves = (int32_t)u32(world + 52);
    scene->face_first = (int32_t)u32(world + 56);
    scene->face_count = (int32_t)u32(world + 60);
    if (scene->face_first < 0 || scene->face_count <= 0 || scene->face_count > FACES_PER_CHUNK * MAX_CHUNKS ||
        !record(scene, FACES, scene->face_first, 20) || (size_t)scene->face_first + scene->face_count > scene->lumps[FACES].len / 20 ||
        !record(scene, NODES, scene->world_root, 24)) return false;
    scene->chunk_count = (scene->face_count + FACES_PER_CHUNK - 1) / FACES_PER_CHUNK;
    return true;
}

static int find_leaf(bsp30_scene *scene, const float p[3])
{
    int node = scene->world_root;
    for (int depth = 0; node >= 0 && depth < 128; ++depth) {
        const uint8_t *n = record(scene, NODES, node, 24);
        if (!n) return -1;
        const uint8_t *plane = record(scene, PLANES, (int32_t)u32(n), 20);
        if (!plane) return -1;
        float side = f32(plane) * p[0] + f32(plane + 4) * p[1] +
                     f32(plane + 8) * p[2] - f32(plane + 12);
        node = (int16_t)u16(n + (side >= 0 ? 4 : 6));
    }
    return node < 0 && record(scene, LEAVES, -1 - node, 28) ? -1 - node : -1;
}

bool bsp30_point_open(bsp30_scene *scene, const float p[3])
{
    const uint8_t *l = record(scene, LEAVES, find_leaf(scene, p), 28);
    return l && (int32_t)u32(l) != -2;
}

static const uint8_t *miptex(bsp30_scene *scene, unsigned index, size_t *remaining)
{
    span lump = scene->lumps[TEXTURES];
    if (lump.len < 4 || index >= u32(lump.data) || index >= (lump.len - 4) / 4) return NULL;
    size_t offset = u32(lump.data + 4 + index * 4);
    if (offset > lump.len || lump.len - offset < 40) return NULL;
    *remaining = lump.len - offset;
    return lump.data + offset;
}

static void load_textures(bsp30_scene *scene)
{
    span lump = scene->lumps[TEXTURES];
    unsigned count = lump.len >= 4 ? u32(lump.data) : 0;
    if (count > MAX_TEXTURES) count = MAX_TEXTURES;
    if (!count) return;
    scene->palettes = heap_caps_malloc(count * sizeof(*scene->palettes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scene->palettes) return;
    scene->texture_count = count;
    for (unsigned i = 0; i < count; ++i) {
        size_t available;
        const uint8_t *header = miptex(scene, i, &available);
        if (!header) continue;
        size_t w = u32(header + 16), h = u32(header + 20);
        size_t base = u32(header + 24), last = u32(header + 36);
        if (w < 8 || h < 8 || w > 1024 || h > 1024 || base < 40 || base > available ||
            w * h > available - base || last < base + w * h || last > available) continue;
        size_t mip_size = (w / 8) * (h / 8);
        if (available - last < mip_size + 2 + 768) continue;
        const uint8_t *palette = header + last + mip_size;
        if (u16(palette) != 256) continue;
        palette += 2;
        for (int c = 0; c < 256; ++c) {
            uint8_t r = palette[c * 3], g = palette[c * 3 + 1], b = palette[c * 3 + 2];
            scene->palettes[i][c] = (r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3;
        }
        scene->textures[i] = (e3d_texture){w, h, header + base, scene->palettes[i], header[0] == '{'};
    }
}

static bool face_position(bsp30_scene *scene, const uint8_t *face, int index, float point[3])
{
    int32_t first = (int32_t)u32(face + 4);
    if (first < 0 || first > INT32_MAX - index) return false;
    const uint8_t *signed_edge = record(scene, SURFEDGES, first + index, 4);
    if (!signed_edge) return false;
    int32_t edge = (int32_t)u32(signed_edge);
    if (edge == INT32_MIN) return false;
    const uint8_t *ends = record(scene, EDGES, abs(edge), 4);
    if (!ends) return false;
    const uint8_t *vertex = record(scene, VERTICES, u16(ends + (edge >= 0 ? 0 : 2)), 12);
    if (!vertex) return false;
    point[0] = f32(vertex) * scale;
    point[1] = f32(vertex + 8) * scale;
    point[2] = -f32(vertex + 4) * scale;
    return isfinite(point[0]) && isfinite(point[1]) && isfinite(point[2]);
}

static bool reverse_face(bsp30_scene *scene, const uint8_t *face, const float (*pos)[3], int edges)
{
    const uint8_t *plane = record(scene, PLANES, u16(face), 20);
    if (!plane) return false;
    const float sign = u16(face + 2) ? -1 : 1;
    const float normal[3] = { f32(plane) * sign, f32(plane + 8) * sign, -f32(plane + 4) * sign };
    for (int i = 1; i + 1 < edges; ++i) {
        const float *a = pos[0], *b = pos[i], *c = pos[i + 1];
        const float u[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        const float v[3] = {c[0] - b[0], c[1] - b[1], c[2] - b[2]};
        float dot = normal[0] * (u[1] * v[2] - u[2] * v[1]) +
                    normal[1] * (u[2] * v[0] - u[0] * v[2]) +
                    normal[2] * (u[0] * v[1] - u[1] * v[0]);
        if (fabsf(dot) > .00001f) return dot < 0;
    }
    return false;
}

static bool hide_face(bsp30_scene *scene, const uint8_t *info, bool vehicle)
{
    size_t remaining;
    const uint8_t *name = info ? miptex(scene, u32(info + 32), &remaining) : NULL;
    return name && (!strncmp((const char *)name, "clip", 5) ||
                    !strncmp((const char *)name, "origin", 7) ||
                    !strncmp((const char *)name, "aaatrigger", 11) ||
                    (vehicle && !strncmp((const char *)name, "glass", 5)));
}

static e3d_lightmap face_lightmap(bsp30_scene *scene, const uint8_t *face,
                                  const uint8_t *info, const float (*uv)[2], int vertices)
{
    const span light = scene->lumps[LIGHTING];
    const size_t offset = u32(face + 16);
    if (!info || (u32(info + 36) & 1) || face[12] == 255 || offset >= light.len)
        return (e3d_lightmap){0};
    int low[2], high[2];
    for (int axis = 0; axis < 2; ++axis) {
        float lo = uv[0][axis], hi = lo;
        for (int i = 1; i < vertices; ++i) {
            lo = fminf(lo, uv[i][axis]);
            hi = fmaxf(hi, uv[i][axis]);
        }
        low[axis] = floorf(lo / 16.f);
        high[axis] = ceilf(hi / 16.f);
    }
    unsigned w = high[0] - low[0] + 1, h = high[1] - low[1] + 1;
    if (w > UINT16_MAX || h > UINT16_MAX || w > (light.len - offset) / 3 / h)
        return (e3d_lightmap){0};
    return (e3d_lightmap){.width = w, .height = h, .rgb = light.data + offset,
                         .min_u = low[0] * 16, .min_v = low[1] * 16};
}

static e3d_mesh *build_mesh(bsp30_scene *scene, int first, int end, bool vehicle, bool world)
{
    int vertices = 0, tris = 0;
    for (int i = first; i < end; ++i) {
        const uint8_t *face = record(scene, FACES, i, 20);
        if (!face) return NULL;
        int edges = u16(face + 8);
        if (edges < 3 || edges > 128) return NULL;
        vertices += edges;
        tris += edges - 2;
    }
    e3d_mesh *mesh = e3d_mesh_create(vertices, tris);
    if (!mesh) return NULL;
    float (*uv)[2] = heap_caps_malloc(vertices * sizeof(*uv), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const e3d_lightmap **lightmaps = heap_caps_calloc(tris, sizeof(*lightmaps),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mesh->uv = (const float (*)[2])uv;
    mesh->lightmaps = lightmaps;
    if (!uv || !lightmaps) goto fail;
    mesh->textures = scene->textures;
    mesh->texture_count = scene->texture_count;
    int vertex = 0, tri = 0;
    for (int i = first; i < end; ++i) {
        const uint8_t *face = record(scene, FACES, i, 20);
        int edges = u16(face + 8);
        const uint8_t *info = record(scene, TEXINFO, u16(face + 10), 40);
        if (hide_face(scene, info, vehicle)) {
            if (world) scene->face_triangle_end[i - scene->face_first] = tri;
            continue;
        }
        unsigned material = info ? u32(info + 32) : 0;
        unsigned shade = vehicle ? 238 : 208;
        size_t light_offset = u32(face + 16);
        if (light_offset <= scene->lumps[LIGHTING].len && scene->lumps[LIGHTING].len - light_offset >= 3) {
            const uint8_t *light = scene->lumps[LIGHTING].data + light_offset;
            shade = 116 + (light[0] + light[1] + light[2]) * 139 / 765;
        }
        bool textured = material < scene->texture_count && scene->textures[material].indices;
        scene->textured_surfaces += textured;
        for (int j = 0; j < edges; ++j) {
            float *p = mesh->pos[vertex + j];
            if (!face_position(scene, face, j, p)) goto fail;
            for (int k = 0; k < 2; ++k) {
                const uint8_t *axis = info ? info + 16 * k : NULL;
                float texel = axis ? (f32(axis) * p[0] - f32(axis + 4) * p[2] +
                                     f32(axis + 8) * p[1]) / scale + f32(axis + 12) : 0;
                uv[vertex + j][k] = isfinite(texel) && fabsf(texel) < 1000000 ? texel : 0;
            }
        }
        e3d_lightmap *lightmap = &scene->face_lightmaps[i];
        *lightmap = face_lightmap(scene, face, info, mesh->uv + vertex, edges);
        const bool lit = textured && lightmap->rgb;
        scene->lightmapped_surfaces += lit;
        /* Use the side of the BSP plane facing walkable space for culling. */
        bool flip = reverse_face(scene, face, mesh->pos + vertex, edges);
        for (int j = 1; j + 1 < edges; ++j) {
            lightmaps[tri] = lit ? lightmap : NULL;
            mesh->tris[tri++] = (e3d_tri){
                .v = {vertex, vertex + j + flip, vertex + j + !flip},
                .texture = textured ? material + 1 : 0,
                .r = lit ? 255 : textured ? shade : shade * (130 + (material * 31u & 63u)) / 255,
                .g = shade * 143 / 255, .b = shade * 158 / 255,
            };
        }
        if (world) scene->face_triangle_end[i - scene->face_first] = tri;
        vertex += edges;
    }
    mesh->nverts = vertex;
    mesh->ntris = tri;
    if (tri) e3d_mesh_finish(mesh, 0);
    return mesh;

fail:
    bsp30_mesh_free(mesh);
    return NULL;
}

static bool build_chunk(bsp30_scene *scene, int chunk)
{
    int first = scene->face_first + chunk * FACES_PER_CHUNK;
    int end = first + FACES_PER_CHUNK;
    if (end > scene->face_first + scene->face_count) end = scene->face_first + scene->face_count;
    e3d_mesh *mesh = build_mesh(scene, first, end, false, true);
    if (!mesh) return false;
    scene->chunks[chunk] = mesh;
    if (!mesh->ntris) return true;
    scene->draw_chunks[chunk] = *mesh;
    scene->draw_chunks[chunk].tris = heap_caps_malloc(mesh->ntris * sizeof(*mesh->tris),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    scene->draw_chunks[chunk].lightmaps = heap_caps_malloc(mesh->ntris * sizeof(*mesh->lightmaps),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scene->draw_chunks[chunk].tris || !scene->draw_chunks[chunk].lightmaps) return false;
    scene->triangles += mesh->ntris;
    return true;
}

static void show_leaf(bsp30_scene *scene, int i)
{
    const uint8_t *entry = record(scene, LEAVES, i, 28);
    if (!entry) return;
    int start = u16(entry + 20), count = u16(entry + 22);
    for (int j = 0; j < count; ++j) {
        const uint8_t *mark = record(scene, MARKS, start + j, 2);
        if (!mark) break;
        int face = u16(mark) - scene->face_first;
        if (face >= 0 && face < scene->face_count) scene->visible[face / 8] |= 1u << (face & 7);
    }
}

static bool pvs(bsp30_scene *scene, int source)
{
    const uint8_t *entry = record(scene, LEAVES, source, 28);
    if (!entry) return false;
    span data = scene->lumps[VISIBILITY];
    size_t cursor = u32(entry + 4);
    if (cursor >= data.len) return false;
    if (scene->world_leaves <= 0 || scene->world_leaves >= scene->lumps[LEAVES].len / 28) return false;
    int count = scene->world_leaves + 1;
    show_leaf(scene, source);
    for (int i = 1; i < count;) {
        if (cursor >= data.len) return false;
        unsigned bits = data.data[cursor++];
        if (!bits) {
            if (cursor >= data.len || !data.data[cursor]) return false;
            i += 8 * data.data[cursor++];
            continue;
        }
        for (int bit = 0; bit < 8 && i < count; ++bit, ++i)
            if (bits & (1u << bit)) show_leaf(scene, i);
    }
    return true;
}

static void prepare_visible_meshes(bsp30_scene *scene, int source)
{
    memset(scene->visible, 0, sizeof(scene->visible));
    if (!pvs(scene, source)) memset(scene->visible, 0xff, sizeof(scene->visible));
    scene->visible_faces = 0;
    for (int c = 0; c < scene->chunk_count; ++c) {
        if (!scene->chunks[c]->ntris) continue;
        int first = c * FACES_PER_CHUNK;
        int end = first + FACES_PER_CHUNK;
        if (end > scene->face_count) end = scene->face_count;
        int triangle = 0, used = 0;
        for (int i = first; i < end; ++i) {
            int next = scene->face_triangle_end[i];
            if (scene->visible[i / 8] & (1u << (i & 7))) {
                int count = next - triangle;
                memcpy(scene->draw_chunks[c].tris + used, scene->chunks[c]->tris + triangle,
                       count * sizeof(*scene->draw_chunks[c].tris));
                memcpy((void *)(scene->draw_chunks[c].lightmaps + used),
                       scene->chunks[c]->lightmaps + triangle, count * sizeof(*scene->chunks[c]->lightmaps));
                used += count;
                scene->visible_faces += count > 0;
            }
            triangle = next;
        }
        scene->draw_chunks[c].ntris = used;
    }
}

void bsp30_mesh_free(e3d_mesh *mesh)
{
    if (!mesh) return;
    free((void *)mesh->uv);
    free((void *)mesh->lightmaps);
    e3d_mesh_free(mesh);
}

void bsp30_close(bsp30_scene *scene)
{
    if (!scene) return;
    for (int i = 0; i < scene->chunk_count; ++i) {
        free(scene->draw_chunks[i].tris);
        free((void *)scene->draw_chunks[i].lightmaps);
        bsp30_mesh_free(scene->chunks[i]);
    }
    free(scene->palettes);
    free(scene->lit_cache);
    free(scene->face_lightmaps);
    free(scene);
}

bsp30_scene *bsp30_open(const void *data, size_t length)
{
    if (!data || length > BSP30_FILE_LIMIT) return NULL;
    bsp30_scene *scene = heap_caps_calloc(1, sizeof(*scene), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scene) return NULL;
    if (!directory(scene, (span){ data, length })) goto fail;
    size_t faces = scene->lumps[FACES].len / 20;
    if (faces > UINT16_MAX) goto fail;
    scene->face_lightmaps = heap_caps_calloc(faces, sizeof(*scene->face_lightmaps),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scene->face_lightmaps) goto fail;
    load_textures(scene);
    for (int i = 0; i < scene->chunk_count; ++i) {
        if (!build_chunk(scene, i)) goto fail;
    }
    scene->leaf = -2;
    return scene;

fail:
    bsp30_close(scene);
    return NULL;
}

const uint8_t *bsp30_entities(const bsp30_scene *scene, size_t *length)
{
    *length = scene->lumps[ENTITIES].len;
    return scene->lumps[ENTITIES].data;
}

e3d_mesh *bsp30_brush_mesh(bsp30_scene *scene, unsigned model, bool omit_glass)
{
    if (!model || model > INT32_MAX) return NULL;
    const uint8_t *brush = record(scene, MODELS, model, 64);
    if (!brush) return NULL;
    unsigned first = u32(brush + 56), faces = u32(brush + 60);
    size_t count = scene->lumps[FACES].len / 20;
    if (!faces || first > count || faces > 512 || faces > count - first || first > INT32_MAX - faces)
        return NULL;
    return build_mesh(scene, first, first + faces, omit_glass, false);
}

void bsp30_prepare_world(bsp30_scene *scene, const float eye[3])
{
    int here = find_leaf(scene, eye);
    if (here == scene->leaf) return;
    scene->leaf = here;
    prepare_visible_meshes(scene, here);
}

size_t bsp30_cache_visible_lightmaps(bsp30_scene *scene, size_t byte_budget)
{
    int end = scene->face_first + scene->face_count;
    if (scene->lit_cache) {
        for (int i = scene->face_first; i < end; ++i) {
            scene->face_lightmaps[i].baked_texels = NULL;
            scene->face_lightmaps[i].baked_texture = NULL;
        }
        free(scene->lit_cache);
        scene->lit_cache = NULL;
    }
    size_t remaining = byte_budget / sizeof(uint16_t);
    if (!remaining || scene->leaf == -2) return 0;
    uint16_t *cache = heap_caps_malloc(remaining * sizeof(*cache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cache) return 0;
    size_t used = 0;
    for (int i = scene->face_first; i < end; ++i) {
        int local = i - scene->face_first;
        if (!(scene->visible[local / 8] & (1u << (local & 7)))) continue;
        e3d_lightmap *lightmap = &scene->face_lightmaps[i];
        size_t count = e3d_lightmap_texel_count(lightmap);
        if (!count || count > remaining) continue;
        const uint8_t *face = record(scene, FACES, i, 20);
        const uint8_t *info = record(scene, TEXINFO, u16(face + 10), 40);
        unsigned image = u32(info + 32); /* A populated lightmap has a valid TEXINFO record. */
        if (image >= scene->texture_count) continue;
        if (!e3d_lightmap_bake(lightmap, &scene->textures[image], cache + used, remaining)) continue;
        used += count;
        remaining -= count;
    }
    if (used) scene->lit_cache = cache;
    else free(cache);
    return used * sizeof(*cache);
}

void bsp30_draw_world(bsp30_scene *scene, e3d_t *engine)
{
    scene->visible_chunks = 0;
    for (int i = 0; i < scene->chunk_count; ++i) {
        if (!scene->draw_chunks[i].ntris) continue;
        e3d_object object;
        e3d_object_init(&object, &scene->draw_chunks[i]);
        object.flags = E3D_UNLIT | E3D_FLAT | E3D_NO_SHADOW;
        e3d_draw(engine, &object);
        ++scene->visible_chunks;
    }
}

bsp30_stats bsp30_get_stats(const bsp30_scene *scene)
{
    return (bsp30_stats){
        .leaf = scene->leaf, .faces = scene->face_count, .triangles = scene->triangles,
        .textured_faces = scene->textured_surfaces, .chunks = scene->chunk_count,
        .lightmapped_faces = scene->lightmapped_surfaces,
        .visible_faces = scene->visible_faces, .visible_chunks = scene->visible_chunks,
    };
}
