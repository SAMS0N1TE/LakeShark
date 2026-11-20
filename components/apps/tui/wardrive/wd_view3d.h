/* WARDRIVE's 3D page: the drive as a ribbon over a ground grid, a pillar
   where each network was heard (colour: what it is, height: how strong), the
   board as an arrow with a ring that goes out with every finished Wi-Fi scan.
   The camera rides behind the board while it moves, swings round it for half
   a minute after it stops, then rests, so a still scene redraws only what
   changed (lap3d's dirty regions). */
#ifndef WD_VIEW3D_H
#define WD_VIEW3D_H

#include <stdbool.h>
#include <stdint.h>
#include "../ls_tui_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draws into `a`; false when the scene could not be made (memory). */
bool wd3d_draw(tui_surface *sf, tui_rect a, uint32_t now_ms);
/* LEFT/RIGHT turn, UP/DOWN tilt, + and - zoom, F follow. */
bool wd3d_key(ls_tk_t k, char ch, uint32_t now_ms);
/* A tap in the 3D area: the outer thirds turn, the middle goes back to following. */
bool wd3d_tap(int col, int row, tui_rect a, uint32_t now_ms);
/* "FOLLOW", "ORBIT", "LOOK" or "REST", for the HUD. */
const char *wd3d_mode(void);
/* The engine's last frame: ms and dots redrawn (for the P key readout). */
void wd3d_stats(unsigned *us, unsigned *dots);
/* Off screen: the scene's memory goes back. */
void wd3d_leave(void);

#ifdef __cplusplus
}
#endif
#endif
