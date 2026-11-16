/* A list that follows the finger. See lap_scroll.h. */
#include "lap_scroll.h"
#include "lakeshark_app.h"

void lap_scroll_stop(lap_scroll_t *s)
{
    s->vel = 0;
    s->frac = 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

bool lap_scroll_follow(lap_scroll_t *s, int *scroll, int smax, int top, int bottom)
{
    int cw = 0, ch = 0;
    lap_cell_px(&cw, &ch);
    if (ch <= 0) ch = 16;
    if (smax < 0) smax = 0;
    const uint32_t now = lap_millis();
    lap_touch_t t;
    if (lap_touch(&t) && t.down) {
        if (!s->held || t.x0 != s->x0 || t.y0 != s->y0) {      /* a new touch */
            s->held = true;
            s->dragging = false;
            s->x0 = t.x0; s->y0 = t.y0;
            s->base = *scroll;
            s->vel = s->frac = 0;
            s->last_y = t.y; s->last_ms = now;
            s->inside = t.y0 >= top * ch && t.y0 < bottom * ch;
        }
        const int dy = t.y0 - t.y, dx = t.x - t.x0;
        const int ady = dy < 0 ? -dy : dy, adx = dx < 0 ? -dx : dx;
        if (!s->dragging && s->inside && ady >= ch * 2 / 3 && ady > adx) {
            s->dragging = true;
            s->base = *scroll;
        }
        if (!s->dragging) return false;
        *scroll = clampi(s->base + (dy + (dy >= 0 ? ch / 2 : -ch / 2)) / ch, 0, smax);
        const uint32_t dt = now - s->last_ms;
        if (dt >= 15) {
            const float v = (float)(s->last_y - t.y) / (float)ch * 1000.0f / (float)dt;
            s->vel = s->vel * 0.4f + v * 0.6f;
            s->last_y = t.y;
            s->last_ms = now;
        }
        return true;
    }
    if (s->held) {
        /* Lifted: a flick carries on; a finger that stopped before lifting does not. */
        const bool was = s->dragging;
        if (!was || now - s->last_ms > 120) s->vel = 0;
        s->held = s->dragging = false;
        s->last_ms = now;
        s->frac = 0;
        if (was) return true;      /* the lift itself is still part of the scroll */
    }
    if (s->vel > 3.0f || s->vel < -3.0f) {
        uint32_t dt = now - s->last_ms;
        if (dt > 100) dt = 100;
        s->last_ms = now;
        s->frac += s->vel * (float)dt / 1000.0f;
        const int step = (int)s->frac;
        s->frac -= (float)step;
        const int v = clampi(*scroll + step, 0, smax);
        if (v == *scroll && step) s->vel = 0;
        *scroll = v;
        s->vel -= s->vel * 2.5f * (float)dt / 1000.0f;
        return s->vel != 0;
    }
    s->vel = 0;
    return false;
}
