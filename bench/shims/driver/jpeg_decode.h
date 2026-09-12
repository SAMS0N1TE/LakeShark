/* Host shim.  The minimal subset of the ESP-IDF hardware JPEG decoder API
   that map_tiles.c references, so the module compiles on the bench.  The
   test binary provides its own implementations of the functions declared
   here in order to inject allocation failures. */
#ifndef LS_SHIM_DRIVER_JPEG_DECODE_H
#define LS_SHIM_DRIVER_JPEG_DECODE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jpeg_decoder_t *jpeg_decoder_handle_t;

typedef enum {
    JPEG_DECODE_OUT_FORMAT_RGB888 = 0,
    JPEG_DECODE_OUT_FORMAT_RGB565 = 1,
    JPEG_DECODE_OUT_FORMAT_GRAY   = 2,
} jpeg_dec_output_format_t;

typedef enum {
    JPEG_YUV_RGB_CONV_STD_BT601 = 0,
    JPEG_YUV_RGB_CONV_STD_BT709 = 1,
} jpeg_yuv_rgb_conv_std_t;

typedef enum {
    JPEG_DEC_RGB_ELEMENT_ORDER_BGR = 0,
    JPEG_DEC_RGB_ELEMENT_ORDER_RGB = 1,
} jpeg_dec_rgb_element_order_t;

typedef enum {
    JPEG_DEC_ALLOC_INPUT_BUFFER  = 0,
    JPEG_DEC_ALLOC_OUTPUT_BUFFER = 1,
} jpeg_dec_buffer_alloc_direction_t;

typedef struct {
    int intr_priority;
    int timeout_ms;
} jpeg_decode_engine_cfg_t;

typedef struct {
    jpeg_dec_buffer_alloc_direction_t buffer_direction;
} jpeg_decode_memory_alloc_cfg_t;

typedef struct {
    jpeg_dec_output_format_t     output_format;
    jpeg_dec_rgb_element_order_t rgb_order;
    jpeg_yuv_rgb_conv_std_t      conv_std;
} jpeg_decode_cfg_t;

esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *cfg,
                                  jpeg_decoder_handle_t *ret_decoder);
esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t decoder_engine);
esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t decoder_engine,
                               const jpeg_decode_cfg_t *decode_cfg,
                               const uint8_t *bit_stream, uint32_t stream_size,
                               uint8_t *decode_outbuf, uint32_t outbuf_size,
                               uint32_t *out_size);
void *jpeg_alloc_decoder_mem(size_t size,
                             const jpeg_decode_memory_alloc_cfg_t *mem_cfg,
                             size_t *allocated_size);

#ifdef __cplusplus
}
#endif

#endif
