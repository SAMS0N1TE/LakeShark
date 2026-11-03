#ifndef LAP_LOOP_H
#define LAP_LOOP_H
#include <stdint.h>
#include <stdbool.h>
/* Caller-owned state; no clock reads, allocation, or firmware imports. */
typedef struct { uint32_t last_ms, pending; unsigned hz, max_steps; bool paused; } lap_loop_t;
bool lap_loop_init(lap_loop_t *loop, unsigned hz, unsigned max_steps, uint32_t now_ms);
unsigned lap_loop_advance(lap_loop_t *loop, uint32_t now_ms, float *alpha);
void lap_loop_pause(lap_loop_t *loop, bool paused, uint32_t now_ms);
/* Pass the focus state from API 2.3 system events. Older targets do nothing. */
void lap_loop_focus(lap_loop_t *loop, bool focused, uint32_t now_ms, unsigned api_major, unsigned api_minor);
#if defined(LAP_API_23)
/* Include lakeshark_app.h first. Feed each polled input event without consuming
   the application's queue here. This adapter imports no runtime functions. */
static inline void lap_loop_event(lap_loop_t *loop, const lap_input_event_t *event, uint32_t now_ms) {
    if (event->kind == LAP_INPUT_FOCUS_LOST || event->kind == LAP_INPUT_PAUSE)
        lap_loop_focus(loop, false, now_ms, LAP_API_MAJOR, LAP_API_MINOR);
    else if (event->kind == LAP_INPUT_FOCUS_GAINED || event->kind == LAP_INPUT_RESUME)
        lap_loop_focus(loop, true, now_ms, LAP_API_MAJOR, LAP_API_MINOR);
}
#endif
#endif
