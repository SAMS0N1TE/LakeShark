#ifndef CARTOCORE_CORE_H
#define CARTOCORE_CORE_H
#define CC_EMBED_API_VERSION 2
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *data; size_t size, used, peak; } cc_arena;
void cc_arena_init(cc_arena *a, void *buffer, size_t size);
void *cc_arena_alloc(cc_arena *a, size_t bytes, size_t alignment);
static inline void *cc_arena_array(cc_arena *a,size_t n,size_t size,size_t alignment) {
    if(size && n>SIZE_MAX/size) return NULL;
    return cc_arena_alloc(a,n*size,alignment);
}
typedef struct { const uint8_t *data; size_t size; } cc_str;
int cc_str_eq(cc_str s, const char *literal);
typedef struct { int32_t x, y; } cc_point;
typedef struct { cc_point *points; size_t count; int closed;
    const uint8_t *xs,*ys; int32_t ox,oy;
} cc_path;
/* Mapped int16 SoA, packed12 pairs (xs non-NULL, ys NULL), or caller points. */
static inline cc_point cc_path_point(const cc_path *p,size_t i) {
    if(!p->xs) return p->points[i];
    if(!p->ys) {
        const uint8_t *v=p->xs+3*i;
        return (cc_point){p->ox+(int32_t)((unsigned)v[0]|((unsigned)v[1]&15)<<8)-8,
                          p->oy+(int32_t)((unsigned)v[1]>>4|(unsigned)v[2]<<4)-8};
    }
    const uint8_t *x=p->xs+2*i,*y=p->ys+2*i;
    return (cc_point){p->ox+(int16_t)((unsigned)x[0]|(unsigned)x[1]<<8),
                      p->oy+(int16_t)((unsigned)y[0]|(unsigned)y[1]<<8)};
}
typedef enum {
    CC_NONE, CC_LAND, CC_PARK, CC_WATER, CC_BUILDING,
    CC_MINOR, CC_MAJOR, CC_BOUNDARY, CC_RAIL, CC_WOOD, CC_FARM, CC_RESIDENTIAL, CC_INDUSTRIAL, CC_MOTORWAY, CC_SECONDARY, CC_SERVICE, CC_CONTOUR, CC_INDEX_CONTOUR, CC_INK_COUNT
} cc_ink;
typedef struct { uint8_t fill, line, priority; } cc_style;
cc_style cc_style_lookup(cc_str layer, cc_str kind);
uint32_t cc_palette(uint8_t ink);
uint8_t cc_palette16(uint8_t ink);
/* RGB stays in bits 0..23; optional semantic ANSI index in bits 24..27. */
uint32_t cc_style_colour(uint8_t ink, int colors16);
static inline uint32_t cc_rgb(uint32_t colour) { return colour & 0xffffff; }
static inline uint8_t cc_index16(uint32_t colour) { return (uint8_t)(colour >> 24) & 15; }
typedef struct cc_feature {
    struct cc_feature *next,*label_next;
    cc_path *paths;
    size_t count;
    uint32_t type;
    cc_style style;
    cc_str name;
    uint8_t label_class, raster_format, terrain_feature;
    cc_point anchor;
    cc_point bounds_min,bounds_max;
    int bounds_valid;
    const uint8_t *raster; /* optional offline polygon phase tables */
    cc_point path_offset;
    uint64_t name_hash; int name_length;
} cc_feature;
static inline cc_point cc_feature_point(const cc_feature *f,const cc_path *p,size_t i) {
    cc_point q=cc_path_point(p,i); q.x+=f->path_offset.x; q.y+=f->path_offset.y; return q;
}
typedef struct cc_terrain { struct cc_terrain *next; const uint8_t *runs; int32_t ox,oy; uint8_t sx,sy,zoom_shift,clipped; int32_t clip[4]; } cc_terrain;
typedef struct {
    cc_terrain *terrain;
    cc_feature *first[16], *last[16]; size_t features, points;
    cc_feature *label_first[6][16],*label_last[6][16]; int labels_indexed;
} cc_scene;
/* Scale source geometry, keeping stroke/text size fixed. Caller-owned copies. */
int cc_scene_overzoom(cc_scene *scene,unsigned shift,int32_t rx,int32_t ry,cc_arena *arena);
typedef enum { CC_EDGES_DEFAULT, CC_EDGES_SMOOTH, CC_EDGES_CRISP } cc_edges;
/* RV32 uses native words for cell-centre and collision bit rows. Dot coverage
   retains its independent 64-bit layout for the braille transpose API. */
#if defined(CC_BITWORD32) || (defined(__riscv) && __riscv_xlen == 32)
typedef uint32_t cc_bitword;
#define CC_WORD_BITS 32
#define CC_WORD_MAX UINT32_MAX
#else
typedef uint64_t cc_bitword;
#define CC_WORD_BITS 64
#define CC_WORD_MAX UINT64_MAX
#endif
typedef struct {
    const uint32_t *palette; /* optional CC_INK_COUNT caller-owned RGB colours */
    int colors16; /* semantic palette, alongside RGB */
    int terrain_strength,terrain_active; /* 0 off, 1..100 strength */
    uint8_t *shade; /* 0..15 shade, 16 absent */
    int terrain_phase_x,terrain_phase_y; /* world cell phase modulo four */
    uint32_t terrain_colours[CC_INK_COUNT][16];
    int terrain_palette_strength,terrain_palette16; const uint32_t *terrain_palette_source;
    cc_edges edges_mode;
    int32_t cols, rows;
    int native_unclipped_lines; /* bounded native-tile paths only */
    int ordered; /* raster owns this flag while traversing priority buckets */
    uint8_t *dots, *ink, *dot_priority, *fill, *fill_priority;
    cc_bitword *edges;
    uint64_t *dot_edges; cc_bitword *collision;
    uint8_t *edge_cells, *coverage, *coverage_ink, *coverage_bg, *coverage_priority;
    const cc_feature **label_seen;
    size_t label_capacity;
    uint32_t *edge_list;
    size_t words;
} cc_planes;
int cc_label_reserve(cc_planes *p, int32_t x, int32_t y, int32_t length);
int cc_planes_init(cc_planes *p, cc_arena *a, int32_t cols, int32_t rows);
/* Crisp working set in hot; smooth-only coverage scratch in cold. */
int cc_planes_init_split(cc_planes *p,cc_arena *hot,cc_arena *cold,int32_t cols,int32_t rows);
void cc_planes_clear(cc_planes *p);
uint8_t cc_dot_bit(int32_t x, int32_t y);
void cc_dot(cc_planes *p, int32_t x, int32_t y, uint8_t ink, uint8_t priority);
int cc_clip_line(int32_t width, int32_t height, cc_point *a, cc_point *b);
void cc_line(cc_planes *p, cc_point a, cc_point b, uint8_t ink, uint8_t priority);
void cc_fill(cc_planes *p, const cc_path *paths, size_t count, uint8_t ink, uint8_t priority);
/* Scalar geometry reference can be selected for bit-exact verification. */
extern int cc_native_rasters;
/* Paths contain dot coordinates (2x4 per cell); closed rings of one feature
   share the even-odd fill. Samples are at cell centres (2*x+1,4*y+2).
   Highest priority wins, equal priority uses the last writer. */
void cc_raster(cc_planes *p, const cc_scene *scene);
void cc_terrain_blit(cc_planes *p,const cc_scene *scene);
void cc_terrain_palette(cc_planes *p);
/* Q16.16 affine transform, floor toward negative infinity. */
int cc_project(int64_t x, int32_t scale_q16, int32_t offset_q16, int32_t *out);
#endif
