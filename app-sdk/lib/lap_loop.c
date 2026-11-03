#include "lap_loop.h"
bool lap_loop_init(lap_loop_t *l, unsigned hz, unsigned max, uint32_t now) {
    if (!l || !hz || hz > 1000 || !max || max > 1000) return false;
    *l = (lap_loop_t){.last_ms=now, .hz=hz, .max_steps=max}; return true;
}
void lap_loop_pause(lap_loop_t *l, bool pause, uint32_t now) {
    l->paused=pause; l->last_ms=now; l->pending=0;
}
void lap_loop_focus(lap_loop_t *l, bool focus, uint32_t now, unsigned major, unsigned minor) {
    if ((major > 2 || (major == 2 && minor >= 3)) && l->paused == focus) lap_loop_pause(l, !focus, now);
}
unsigned lap_loop_advance(lap_loop_t *l, uint32_t now, float *alpha) {
    uint32_t dt=now-l->last_ms; l->last_ms=now;
    if (l->paused) { if (alpha) *alpha=0; return 0; }
    /* Bound catch-up, including suspend and a wrapping millisecond clock. */
    if (dt > 1000) dt=1000;
    l->pending += dt*l->hz;
    unsigned n=l->pending/1000;
    l->pending %= 1000;
    if (alpha) *alpha=l->pending/1000.0f;
    return n > l->max_steps ? l->max_steps : n;
}
