#ifndef CARTOCORE_OUT_H
#define CARTOCORE_OUT_H
#include "core.h"
typedef struct { uint32_t codepoint, fg, bg; } cc_cell;
/* ANSI mode argument: 0=256, 1=truecolor (legacy), 16=semantic ANSI-16. */
#define CC_ANSI_16 16
typedef enum { CC_BRAILLE, CC_QUADRANT, CC_HALF, CC_ASCII } cc_mode;
/* Central defaults, editable by embedders at startup. */
extern cc_edges cc_edge_defaults[4];
cc_edges cc_edges_resolve(cc_edges edges, cc_mode mode);
typedef struct { uint8_t ascii[256]; } cc_encoder;
void cc_encoder_init(cc_encoder *e);
/* Shared glyph vocabulary; mask is row-major at each mode's sample grid. */
uint32_t cc_encoder_glyph(const cc_encoder *e,cc_mode mode,unsigned braille_bits);
unsigned cc_encoder_mask(cc_mode mode,uint32_t codepoint);
unsigned cc_media_bits(cc_mode mode,unsigned mask);
void cc_encode(const cc_encoder *e, const cc_planes *p, cc_mode mode, cc_cell *cells);
/* One cols-sized scratch row; cells receives contiguous copies. */
void cc_encode_staged(const cc_encoder *e,const cc_planes *p,cc_mode mode,cc_cell *cells,cc_cell *scratch);
void cc_encode_list(const cc_encoder *e,const cc_planes *p,cc_mode mode,cc_cell *cells,const uint32_t *list,size_t count);
void cc_encode_mask(const cc_encoder *e,const cc_planes *p,cc_mode mode,cc_cell *cells,const uint8_t *mask);
void cc_labels(cc_planes *p, const cc_scene *scene, cc_cell *cells);
void cc_label_metadata(cc_feature *feature);

typedef struct { char *data; size_t capacity, size; int error; } cc_buffer;
void cc_buffer_init(cc_buffer *b, char *data, size_t capacity);
/* Output accumulated in caller buffer; previous may be NULL. */
int cc_ansi(cc_buffer *b, const cc_cell *cells, const cc_cell *previous,
            int32_t cols, int32_t rows, int truecolor);
typedef struct { uint32_t fg,bg; int valid,truecolor; } cc_ansi_state;
/* Strict changed-cell writer, persistent SGR state owned by caller. */
int cc_ansi_diff(cc_buffer *b,const cc_cell *cells,const cc_cell *previous,
    int32_t cols,int32_t rows,int truecolor,cc_ansi_state *state);
int cc_html_raster(cc_buffer *b,const cc_cell *cells,int32_t cols,int32_t rows);
int cc_html(cc_buffer *b, const cc_cell *cells, int32_t cols, int32_t rows);
#endif
