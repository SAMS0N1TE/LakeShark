/* POWER MODE: how hard the board works for the person holding it.

   FULL   everything as the settings have it.
   SAVER  the panel held to half, dimmed after 30 s, the keyboard light down
          with it, and the screen drawn at 15 frames a second.
   WATCH  SAVER, and the screen goes dark after 20 idle seconds. While it is
          dark the UI wakes four times a second instead of twenty-five, draws
          nothing to the panel, and only what WATCH KEEPS goes on running:

          EVERYTHING  nothing is stopped; the screen is just dark.
          MESH        MeshCore on; the GPS reader stops (the module goes to
                      standby) and a radio app on screen is left for HOME.
          P25         the app on screen keeps its radio; the GPS reader and
                      MeshCore stop.
          GPS LOG     the GPS on and recording a track to the card; MeshCore
                      stops and a radio app on screen is left for HOME.

          A touch, a key or a turn of the board lights the screen, and what
          WATCH stopped comes back as it was, the app on screen included.

   The user's own brightness, dim time and keyboard light stay as they were
   saved: a mode lays limits over them and FULL takes the limits away. */

#ifndef LS_POWER_H
#define LS_POWER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LS_PWR_FULL, LS_PWR_SAVER, LS_PWR_WATCH, LS_PWR_MODES } ls_pwr_mode_t;
typedef enum { LS_KEEP_ALL, LS_KEEP_MESH, LS_KEEP_P25, LS_KEEP_GPS, LS_KEEPS } ls_pwr_keep_t;

/* Once at boot, after the settings and the display: the saved mode, applied. */
void ls_power_init(void);

ls_pwr_mode_t ls_power_mode(void);
void ls_power_set_mode(ls_pwr_mode_t mode);          /* applied and saved */
ls_pwr_keep_t ls_power_keep(void);
void ls_power_set_keep(ls_pwr_keep_t keep);          /* saved */

const char *ls_power_mode_name(int mode);            /* "FULL" ...        */
const char *ls_power_mode_what(int mode);            /* one short line    */
const char *ls_power_keep_name(int keep);            /* "EVERYTHING" ...  */

/* From the UI loop, once a frame: notices the screen going dark or lit and
   parks or restores what WATCH keeps. */
void ls_power_tick(void);

/* The frame period the UI loop should keep, given its own: longer under
   SAVER and WATCH, and a quarter second while WATCH has the screen dark. */
int ls_power_frame_ms(int normal_ms);

/* True while the panel is dark under a power mode: the loop may skip
   presenting frames nobody can see. */
bool ls_power_dark(void);

/* What WATCH stopped for the dark, as words ("GPS, FM"), or "" for nothing. */
const char *ls_power_parked(void);

/* The pack current as the gauge read it, one sample a minute, oldest
   first, into `out` (mA, negative discharging). Returns how many; 0 before
   the first sample or on a board without a gauge. */
#define LS_POWER_HISTORY 60
int ls_power_history(short *out, int max);

/* The 'power' console command. */
void ls_power_console_register(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_POWER_H */
