/* RF SURVEY on the board: the receiver, the position, the card and the heat.

   The screen asks (start, stop, replay, export) and reads a snapshot; the
   work happens on the field worker's direction-finding task, which already
   steps every receiver ls_dfs knows. Sources:

     RTL-SDR, HackRF, CC1101, LoRa carrier   ls_dfs channel levels, eight
                                             channels at a time
     LORA SWEEP                              the LoRa chip's RSSI sweep over
                                             the plan's span (SX1262 or LR2021)
     LR2021 IQ                               lr_wf + lr_iq_spectrum, 125 kHz a
                                             look, wide mode optional
     SIM                                     the pretend world (ls_rfsim.h)

   One pass over every channel, at a place, is a row (ls_rfs.h): it goes to
   /sdcard/lakeshark/rfsurvey/<stamp>.lsrf and into the heat grid MAP draws.
   Nothing here runs on the UI task but the snapshot copies. */

#ifndef LS_RFS_RUN_H
#define LS_RFS_RUN_H

#include <stdbool.h>
#include <stdint.h>

#include "ls_rfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_RFS_SRC_RTL = 0, LS_RFS_SRC_HACKRF, LS_RFS_SRC_LORA, LS_RFS_SRC_SWEEP,
    LS_RFS_SRC_LR_IQ, LS_RFS_SRC_CC1101, LS_RFS_SRC_SIM, LS_RFS_SRCS
} ls_rfs_src_t;

const char *ls_rfs_src_name(ls_rfs_src_t s);
/* One line on what the source measures and in what unit. */
const char *ls_rfs_src_detail(ls_rfs_src_t s);
/* NULL when the source can be used now, else why not. */
const char *ls_rfs_src_unavailable(ls_rfs_src_t s);

typedef struct {
    ls_rfs_src_t src;
    ls_rfs_plan_t plan;
    float step_m;            /* a row every this many metres; 0 every pass */
    float cell_m;            /* heat cell side */
    float threshold_db;      /* occupied: this far over the channel's floor */
    uint16_t dwell_ms;       /* per retune, for the ls_dfs sources */
    bool wide;               /* LR2021 IQ: wide mode (576k, LO offset) */
} ls_rfs_settings_t;

void ls_rfs_settings_default(ls_rfs_settings_t *out);

#define LS_RFS_TRACK 512

typedef struct {
    bool running, replaying, busy, have_file;
    ls_rfs_src_t src;
    char src_name[24], unit[8];
    char plan[LS_RFS_NAME + 1];
    int nch;
    ls_rfs_ch_t ch[LS_RFS_CH_MAX];
    /* The last complete pass. */
    float level[LS_RFS_CH_MAX], floor[LS_RFS_CH_MAX];
    uint32_t occ, heard;     /* bit masks */
    uint32_t pass_serial;    /* bumps once per pass */
    int64_t pass_us;
    uint32_t rows, passes, waiting;  /* rows kept; passes; passes with no position */
    int cells; uint32_t dropped;
    float walked_m;
    uint32_t elapsed_s;
    bool where_ok; uint16_t where_flags;
    double lat, lon;
    int group, groups;       /* which eight the receiver is on */
    uint32_t replay_done, replay_total;
    char status[96];
    char file[48];
} ls_rfs_view_t;

/* UI calls: each only leaves a request for the worker. */
bool ls_rfs_start(const ls_rfs_settings_t *s);
void ls_rfs_stop(void);
/* NULL replays the newest file; `speed` rows a second, 0 all at once. */
bool ls_rfs_replay(const char *path, int speed);
/* Writes <file>.csv and <file>.geojson beside the survey file (the current
   or last one when NULL). */
bool ls_rfs_export(const char *path);
/* Forget the heat and the track (not the files). */
void ls_rfs_clear(void);
void ls_rfs_view(ls_rfs_view_t *out);
bool ls_rfs_running(void);
/* Whether the worker should step quickly. */
bool ls_rfs_busy(void);

/* Survey files on the card, newest first, as the worker last listed them;
   ls_rfs_files_refresh asks it to list again. */
void ls_rfs_files_refresh(void);
int  ls_rfs_files(char names[][40], int max);

/* Plans from /sdcard/lakeshark/rfsurvey/plans.json (ls_rfs_plans_parse),
   read with the file list; and why there are none, or NULL. */
#define LS_RFS_CARD_PLANS 8
int  ls_rfs_card_plans(ls_rfs_plan_t *out, int max);
const char *ls_rfs_card_plans_why(void);

/* What MAP and the screen colour the heat by. */
void ls_rfs_show(int ch, ls_rfs_metric_t m);
void ls_rfs_shown(int *ch, ls_rfs_metric_t *m);

/* The heat and the track, held while drawn. Never NULL. Keep it short. */
const ls_rfs_grid_t *ls_rfs_grid_lock(void);
void ls_rfs_grid_unlock(void);
/* Kept rows' places, oldest first, while the grid is locked. */
int  ls_rfs_track(double *lat, double *lon, int max);

/* The worker, from the direction-finding task. */
void ls_rfs_step(void);

/* `rfsurvey` on the console. */
int ls_rfs_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
