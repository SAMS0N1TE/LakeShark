#ifndef CARTOCORE_MVT_H
#define CARTOCORE_MVT_H
#include "core.h"
typedef struct { const uint8_t *p, *end; int error; } cc_wire;
typedef struct { uint32_t number, type; uint64_t integer; cc_str bytes; } cc_field;
cc_wire cc_wire_init(const void *data, size_t size);
int cc_varint(cc_wire *r, uint64_t *out);
int64_t cc_zigzag(uint64_t value);
int cc_wire_next(cc_wire *r, cc_field *field);
/* Wire types 0,1,2,5; deprecated protobuf groups are rejected. */
typedef void (*cc_info_fn)(void *user, cc_str layer, size_t features, cc_str kind);
/* One layer callback (kind.data=NULL), followed by each feature's selected
   string class. Precedence is class, kind, pmap:kind. Borrowed slices. */
int cc_mvt_info(cc_str tile, cc_arena *arena, cc_info_fn fn, void *user);
/* Empty filter includes styled layers. Input must outlive the scene.
   Supported MVT versions: 1,2. Returns 0 on malformed data or arena exhaustion;
   failure restores arena.used and clears scene. Viewport fills the one tile. */
int cc_mvt_decode(cc_str tile, cc_arena *arena, int32_t cols, int32_t rows,
                  cc_str layer_filter, cc_scene *scene);
/* Tile dimensions and origin in Q16.16 viewport dots; input borrowed. */
int cc_mvt_decode_transform(cc_str tile, cc_arena *arena, int64_t width, int64_t height,
                            int32_t ox, int32_t oy, cc_scene *scene);
#endif
