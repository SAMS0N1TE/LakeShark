/* The map trace ('carto trace' on the console), with no other includes, so
   code outside the map can mark it too. Defined in ls_map.c. LS_TRACE marks
   on the board and compiles away in PC builds that do not link ls_map.c. */
#ifndef LS_TRACE_H
#define LS_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ls_map_trace_reset(void);
void ls_map_trace(const char *what, int32_t a, int32_t b);
void ls_map_trace_dump(void);

#ifdef ESP_PLATFORM
#define LS_TRACE(what, a, b) ls_map_trace((what), (int32_t)(a), (int32_t)(b))
/* The clock the marks are timed with; none off the board. */
#define LS_TRACE_US() esp_timer_get_time()
#else
#define LS_TRACE(what, a, b) ((void)(what), (void)(a), (void)(b))
#define LS_TRACE_US() ((int64_t)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* LS_TRACE_H */
