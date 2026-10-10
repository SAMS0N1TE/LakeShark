/* Fresh source-bin accumulator shared by the strip decoders. */
#ifndef CC_MEDIA_STREAM_H
#define CC_MEDIA_STREAM_H
#include "cartocore/media.h"
typedef struct {
    cc_media *media; cc_arena *arena; uint64_t *sum;
    unsigned sw,sh,ow,oh,orientation,scale; cc_media_error error;
    cc_media_stats stats;
    cc_media_layout layout;
} cc_grid_sink;
int cc_grid_begin(cc_grid_sink *s,unsigned w,unsigned h,unsigned orientation,unsigned scale);
void cc_grid_pixel(cc_grid_sink *s,unsigned x,unsigned y,const uint8_t rgb[3]);
void cc_grid_finish(cc_grid_sink *s);
int cc_jpeg_run(const uint8_t *,size_t,cc_arena *,cc_rgb_frame *,cc_grid_sink *);
/* Full reduced frame, also used to verify streamed scaling independently. */
int cc_jpeg_decode_scaled(const uint8_t *,size_t,cc_arena *,cc_rgb_frame *,unsigned);
int cc_png_run(const uint8_t *,size_t,cc_arena *,cc_rgb_frame *,cc_grid_sink *);
#endif
