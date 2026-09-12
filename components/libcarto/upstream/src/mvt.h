#ifndef CARTO_MVT_H
#define CARTO_MVT_H

#include "carto/framebuffer.h"
#include "carto/style.h"
#include "carto/raster.h"
#include "carto/label.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*LS-1067  The largest single ring the scratch has held since the last reset.
   See LS DEVIATION 3 in carto.c: the scratch is sized by assertion, and this
   is how that assertion gets checked against real tiles. */
int  mvt_scratch_peak(void);
void mvt_scratch_peak_reset(void);


/*LS-1050  `val_ptr`, `val_len` and `val_cap` are the layer value table, and
   they are the CALLER's memory.

   They used to be arrays inside this function's own frame - thirty-two
   kilobytes of it - which is fine on a host and a stack overflow on any
   embedded task. See the note in mvt.c. */
void carto_mvt_render_category(carto_framebuffer *fb, const carto_style *style,
                              const uint8_t *tile, size_t len,
                              carto_layer_kind category,
                              double ox, double oy, double tile_px,
                              int zoom,
                              carto_ipt *scratch, int scratch_cap,
                              const uint8_t **val_ptr, int *val_len,
                              int *val_num, int val_cap,
                              carto_label_sink *labels);

#ifdef __cplusplus
}
#endif

#endif
