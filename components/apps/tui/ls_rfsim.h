/* Where am I, for RF SURVEY and FOX HUNT, and a pretend world to test them in.

   ls_rf_where answers with the GPS fix, or with a simulated position when
   one is set from the console: standing at a place (`rfsim here`), or
   walking a lawnmower pattern from a start (`rfsim walk`), so a survey can be
   walked and a fox hunted indoors where the receiver has no sky.

   The pretend world also has transmitters: three around the walk's start,
   each keyed on its own channel of the survey plan, heard with free-space
   loss, a little fading and, for the fox, the body shadow of a board held
   in front of you. The SIM source of RF SURVEY and of FOX HUNT read them, so
   the whole of both apps can be exercised on the bench, and in lssim. */

#ifndef LS_RFSIM_H
#define LS_RFSIM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LS_RFSIM_OFF = 0, LS_RFSIM_HERE, LS_RFSIM_WALK } ls_rfsim_mode_t;

typedef struct {
    double lat, lon;          /* start */
    float course;             /* first leg, true degrees */
    float speed_mps;
    float leg_m, step_m;      /* lawnmower: long legs, short steps across */
} ls_rfsim_walk_t;

/* Pure: where the walk is `t_s` seconds in, and which way it is going. */
void ls_rfsim_walk_at(const ls_rfsim_walk_t *w, double t_s, double *lat, double *lon, float *course);

void ls_rfsim_here(double lat, double lon);
void ls_rfsim_walk(double lat, double lon, float course, float speed_mps);
void ls_rfsim_off(void);
ls_rfsim_mode_t ls_rfsim_mode(void);
/* Puts the fox (transmitter 0) at a place; the others keep theirs. */
void ls_rfsim_fox(double lat, double lon);
bool ls_rfsim_fox_at(double *lat, double *lon);

enum { LS_RF_WHERE_FIX = 1, LS_RF_WHERE_SIM = 2 };   /* = LS_RFS_ROW_FIX / _SIM */
/* The simulated position while one is set, else a current GPS fix. False
   with neither. `flags` gets LS_RF_WHERE_*; `course` NAN when unknown. */
bool ls_rf_where(double *lat, double *lon, uint16_t *flags, float *course);

/* What the pretend receiver hears at a place, in dBm, for a channel `bw_hz`
   wide at `hz`: the noise floor, plus every transmitter keyed on that
   channel. `heading` (true, NAN for none) adds the body shadow. */
float ls_rfsim_level(double lat, double lon, uint32_t hz, uint32_t bw_hz, float heading, int64_t now_us);
/* The fox alone, whatever the frequency: what FOX HUNT's SIM source hears. */
float ls_rfsim_fox_level(double lat, double lon, float heading, int64_t now_us);
/* Ties the transmitters to a plan's channels (0, 2 and 5, as it has them). */
void ls_rfsim_plan(const uint32_t *hz, int n);

/* `rfsim` on the console. */
int ls_rfsim_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
