#ifndef CARTOCORE_ABI_H
#define CARTOCORE_ABI_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
# ifdef CC_ABI_BUILD
#  define CC_API __declspec(dllexport)
# else
#  define CC_API __declspec(dllimport)
# endif
#else
# define CC_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cc_host cc_host;
typedef struct { uint32_t codepoint, fg, bg; } cc_host_cell;
/* ABI 1, cdecl. Modes: braille=0, quadrant=1, half=2, ascii=3.
   Edges: default=0, smooth=1, crisp=2. Depth: 16,256,24 (truecolor).
   Every object is single-owner. All functions except destroy return 0/NULL on
   invalid input/allocation/decode failure. Cells remain valid until next render.
   begin resets the frame; tile bytes are copied and may be released on return.
   World pixels use 256 pixels/tile at viewport zoom; x is unwrapped for dateline.
   ANSI compares the previous successful ANSI call, not the previous render. */
CC_API uint32_t cc_host_abi_version(void);
CC_API cc_host *cc_host_create(int32_t cols,int32_t rows,int32_t mode,int32_t edges,int32_t depth);
CC_API void cc_host_destroy(cc_host *h);
/* Optional 16 RGB colours indexed by cc_ink; NULL restores engine defaults. */
CC_API int cc_host_palette(cc_host *h,const uint32_t *rgb,size_t count);
CC_API int cc_host_begin(cc_host *h,int32_t zoom,double origin_x,double origin_y,double width,double height);
CC_API int cc_host_tile(cc_host *h,int32_t z,int32_t x,int32_t y,const void *bytes,size_t size);
CC_API int cc_host_render(cc_host *h);
CC_API const cc_host_cell *cc_host_cells(cc_host *h,size_t *count);
CC_API const char *cc_host_ansi(cc_host *h,size_t *size);
#ifdef __cplusplus
}
#endif
#endif
