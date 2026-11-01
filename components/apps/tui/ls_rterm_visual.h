/* Optional cells-feed state. All allocations follow the current picture/grid;
   never a maximum desktop framebuffer. Owned by the snapshot handoff. */
#ifndef LS_RTERM_VISUAL_H
#define LS_RTERM_VISUAL_H
#define PICTURE_US 100000
/* Raw (pre-deflate) bytes a tick. A 115x27 MAP window is ~470 KB raw and
   deflates only ~8x (~60 KB): at 128 KiB it arrived in four bands 0.2 s apart.
   512 KiB sends a whole window's picture in one update. */
#include "ls_rterm_budget.h"
#define PICTURE_BYTES LS_RTERM_SERIAL_PICTURE_BYTES
#define VECTOR_CAP 512
#define VECTOR_CELL_CAP 1024
typedef struct { uint16_t x0,y0,x1,y1; uint8_t attr; } visual_line;
typedef struct { uint16_t glyph; uint8_t attr,bits; } visual_dot;
typedef struct {
    bool pixels, vectors;
    tui_rect rect;
    int w,h,x0,y0,sw,sh,cursor;
    uint32_t *hash;
    uint8_t *valid;
    int64_t last_us;
    uint32_t masks_hash, vectors_hash;
    uint32_t masks_pending;
    size_t mask_cursor;
    uint32_t picture_serial;
    int remaining;
    size_t budget;
    int64_t last_picture_us,last_vectors_us;
    bool invalidate,vectors_full;
    uint8_t *columns_valid;
    int pending_columns,column_cursor,column_step;
    uint32_t full_epoch;
    uint32_t ring_columns,ring_rows;
    bool by_columns;
    bool pan_active,pan_full;
    uint32_t palette_generation;
} visual_view;
typedef struct {
    tui_rect rect;
    const uint16_t *identity;
    uint16_t *src;
    size_t bytes;
    int sw,sh,w,h,x0,y0;
    uint32_t serial,generation;
    bool ready,unavailable,ring;
    uint32_t ring_epoch,ring_columns,ring_rows,full_epoch,copy_bytes;
    uint8_t partial_axes;
    bool pan;
    uint32_t pan_epoch,pan_base;
    int32_t pan_left,pan_top,pan_dx,pan_dy;
    uint16_t pan_ground;
    uint16_t *dirty_lo,*dirty_hi;
    uint16_t *palette_lut,*palette;
    unsigned palette_n;
    bool palette_overflow;
    uint32_t palette_generation;
    /* O records, already packed, generated with the board's coverage code. */
    uint8_t *masks;
    size_t masks_n;
    uint32_t masks_hash;
} visual_picture;
static visual_picture s_picture;
static uint32_t s_picture_generation,s_picture_full_epoch,s_palette_generation;
static visual_line *s_lines, *s_frame_lines;
static visual_dot *s_dots, *s_frame_dots;
static size_t s_dot_count, s_frame_dot_count;
static unsigned s_line_count, s_frame_line_count, s_line_dropped;
static uint32_t s_vectors_hash;
static bool s_visual_pixels, s_visual_vectors;
static bool s_trace_new;
static int64_t s_picture_us;
static bool visual_palette_add(uint16_t colour) {
    if(s_picture.palette_lut[colour]!=UINT16_MAX)return true;
    if(s_picture.palette_n==256) {s_picture.palette_overflow=true;return false;}
    s_picture.palette_lut[colour]=(uint16_t)s_picture.palette_n;
    s_picture.palette[s_picture.palette_n++]=colour;return true;
}
static uint32_t visual_hash(const void *p, size_t n) {
    const uint8_t *b=p; uint32_t h=2166136261u;
    while(n--) h=(h^*b++)*16777619u;
    return h;
}
#endif
