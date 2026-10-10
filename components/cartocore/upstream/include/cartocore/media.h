#ifndef CARTOCORE_MEDIA_H
#define CARTOCORE_MEDIA_H
#include "out.h"
typedef struct { const uint8_t *pixels; uint32_t width,height; size_t stride; } cc_rgb_frame;
typedef enum { CC_DITHER_AUTO, CC_DITHER_NONE, CC_DITHER_BAYER, CC_DITHER_BLUE, CC_DITHER_DIFFUSION } cc_dither;
typedef enum { CC_MEDIA_FIT, CC_MEDIA_FILL, CC_MEDIA_STRETCH } cc_media_sizing;
typedef struct {
    cc_mode mode; int cols,rows,colors,video; /* colors: 1=truecolor, 0=256, 16=ANSI16 */
    cc_dither dither;
    /* Set mode/cols/rows at init; dither/colors/video/stability/sizing/cell_aspect may change.
       Mean squared RGB error allowance per sample for keeping the old glyph. */
    unsigned stability;
    cc_media_sizing sizing;
    double cell_aspect; /* Cell width/height; zero selects the default 0.5. */
} cc_media_options;
typedef struct { unsigned sx,sy,sw,sh,dx,dy,dw,dh; } cc_media_layout;
typedef struct {
    cc_media_options options; cc_encoder encoder;
    uint8_t *scaled,*masks; int32_t *diffusion;
    uint32_t *xb,*yb; int width,height;
} cc_media;
int cc_media_init(cc_media *m,cc_arena *a,cc_media_options options);
/* Shared integer source/destination rectangles; aspect dimensions precede IDCT reduction. */
cc_media_layout cc_media_geometry(const cc_media *m,unsigned sw,unsigned sh,unsigned aw,unsigned ah);
/* Previous cells optional; video never uses error diffusion. No allocation. */
int cc_media_render(cc_media *m,cc_rgb_frame frame,const cc_cell *previous,cc_cell *cells);
typedef enum { CC_MEDIA_OK, CC_MEDIA_MALFORMED, CC_MEDIA_TRUNCATED,
    CC_MEDIA_OOM, CC_MEDIA_PROGRESSIVE, CC_MEDIA_UNSUPPORTED } cc_media_error;
typedef struct { size_t arena_bytes; unsigned width,height,orientation,scale; } cc_media_stats;
/* Decode directly to target dots. Scratch is released on return; no full image.
   JPEG chooses a reduced IDCT retaining at least twice the target resolution.
   Input bytes remain owned by the caller. */
cc_media_error cc_media_decode_grid(const uint8_t *data,size_t size,cc_arena *a,
    cc_media *m,cc_media_stats *stats);
const char *cc_media_error_string(cc_media_error error);
/* Strict baseline: PNG noninterlaced gray/RGB/palette/gray-alpha/RGBA, all standard depths;
   JPEG SOF0 gray, 4:4:4, 4:2:2 or 4:2:0, interleaved sequential scan. Alpha over black.
   Failure restores the arena. Output and scratch both belong to caller arena. */
int cc_media_decode(const uint8_t *data,size_t size,cc_arena *a,cc_rgb_frame *frame);
int cc_png_decode(const uint8_t *data,size_t size,cc_arena *a,cc_rgb_frame *frame);
int cc_jpeg_decode(const uint8_t *data,size_t size,cc_arena *a,cc_rgb_frame *frame);
int cc_media_cli(int argc,char **argv);
#endif
