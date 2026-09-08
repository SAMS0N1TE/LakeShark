#ifndef REC_SCOUT_SPAN_H
#define REC_SCOUT_SPAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*LS-560  SCOUT's span ladder is shared by the LVGL controls and the host
   bench.  Keeping the boundary arithmetic here prevents ZOOM + from wrapping
   from the narrowest view back to the widest, which is useful for the SPAN
   cycle but wrong for a directional zoom control. */
#define REC_SCOUT_SPAN_LEVEL_COUNT 4

uint32_t rec_scout_span_hz(int level);
int rec_scout_span_cycle(int level);
int rec_scout_span_zoom_in(int level);
int rec_scout_span_zoom_out(int level);

#ifdef __cplusplus
}
#endif

#endif
