/* A real screenshot: the panel's own pixels, as a PNG, over the console. */

#ifndef LS_TUI_PNG_H
#define LS_TUI_PNG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ls_tui_png_line_fn)(const char *line, void *ctx);

/* `px` is the native framebuffer, RGB565, `native_w` x `native_h`, row-major.
   `landscape` reads it back through the blitter's clockwise transpose so the
   PNG comes out in the logical orientation. Each line handed to `emit` is at
   most 76 base64 characters. Returns the PNG's size in bytes and its CRC-32
   in *crc_out, or 0 if the encoder could not get its working memory or the
   compressor failed. */
size_t ls_tui_png_emit(const uint16_t *px, int native_w, int native_h,
                       bool landscape, ls_tui_png_line_fn emit, void *ctx,
                       uint32_t *crc_out);

#ifdef __cplusplus
}
#endif
#endif
