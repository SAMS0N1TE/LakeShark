/* Things that move, in one place. */

#ifndef LS_MOTION_H
#define LS_MOTION_H

#include <stdbool.h>
#include <stdint.h>

#include "ls_tui.h"

#ifdef __cplusplus
extern "C" {
#endif

int ls_motion_phase(int steps, int period_ms);

/* A one cell sign of life. */

char ls_motion_pip(bool live);
/* Small activity indicator in a panel border; never implies measured progress. */
void ls_motion_busy(tui_surface *sf, tui_rect panel, bool active);

/* How recently a value changed, 255 down to 0. */

typedef struct {
    uint32_t token;
    int64_t  since_us;
    bool     seeded;
} ls_fresh_t;

uint8_t ls_fresh(ls_fresh_t *f, uint32_t token, int fade_ms);

/* The same, without a token: something happened, now. */
void    ls_fresh_bump(ls_fresh_t *f);
uint8_t ls_fresh_level(const ls_fresh_t *f, int fade_ms);

/* An attribute that cools down.

   `hot` while the level is high, `cool` once it has faded, and the step
   between them is deliberately one step and not a gradient: the palette has
   eight colours and two intensities, so a "gradient" would be three visible
   jumps pretending to be smooth. One clean change that decays to normal
   reads as an event; three jumps read as a fault. */
uint8_t ls_fresh_attr(uint8_t level, uint8_t hot_fg, uint8_t cool_fg,
                      uint8_t bg);

#ifdef __cplusplus
}
#endif

#endif /* LS_MOTION_H */
