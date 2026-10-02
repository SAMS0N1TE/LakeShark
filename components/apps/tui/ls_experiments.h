/* EXPERIMENTS: radio work that might one day be an app of its own.

   Each experiment is one file, experiments/exp_<id>.c, defining one
   `const ls_experiment_t exp_<id>`, and one line in
   ls_experiments_builtin.c, EXP(exp_<id>), registers it.
   The EXPERIMENTS tile lists them with how far along each is; opening one
   gives START/STOP, RADIO, OPTIONS when it has any, and its live readout.
   The `exp` console command reaches the same list.

   One experiment runs at a time, on one worker task ("exp", a 6 KB stack in
   PSRAM, priority 2) that the framework owns:

     - start() runs on the worker after the framework has taken the LoRa
       socket from MeshCore (ls_mesh_radio_hold, retried for a second) and
       checked that nothing else is driving it: LoRa Labs DIRECT, an FSK
       session, a sweep or a Mode S session. Starting another experiment
       stops the running one first; starting the running one restarts it,
       which is how a changed setting takes effect.
     - poll() runs on the worker about every 2 ms while running. It does the
       radio work and returns quickly; it never waits on the UI.
     - stop() runs on the worker, then the socket goes back to MeshCore.
     - lines() runs on the UI task and on the console, at the same time as
       poll(): copy out of state the worker writes, under the module's own
       lock (a portMUX critical section is enough for a few numbers).
     - configure() and the OPTIONS setters run on the caller's task. They
       change what the next start() uses; to apply a change to a running
       experiment, call ls_exp_start() on it again.

   The worker's stack is in PSRAM, so start, poll and stop must not touch
   NVS or flash (no settings_get/set, no nvs_*): that asserts in
   cache_utils.c. Read settings on the UI task or at boot. Keep large
   buffers out of poll's frame: static and EXT_RAM_BSS_ATTR. */

#ifndef LS_EXPERIMENTS_H
#define LS_EXPERIMENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ls_options.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How far along it is, shown as a badge: IDEA, TRYING, WORKS. */
typedef enum { LS_EXP_IDEA, LS_EXP_TRYING, LS_EXP_WORKS } ls_exp_maturity_t;

/* One line of a live readout, with its terminator. */
#define LS_EXP_LINE 52

typedef struct ls_experiment {
    const char *id;     /* console name, lower case, e.g. "dfm17" */
    const char *name;   /* tile/list title, e.g. "RADIOSONDE" */
    const char *sub;    /* one line: what it hears and where, e.g. "DFM-17 balloons, 400-406 MHz" */
    ls_exp_maturity_t maturity;
    const char *needs;  /* optional: what it needs that may be missing ("a UAT aircraft in range"), or NULL */
    /* Called on the experiments worker task. start returns false and writes why into `why` when it cannot run. */
    bool (*start)(char *why, size_t why_len);
    void (*stop)(void);
    /* Called on the worker task about every 2 ms while running; does the radio work; must not block long. */
    void (*poll)(void);
    /* Live readout: fill up to `max` lines of up to LS_EXP_LINE-1 chars; return the count. Called from the UI
       task and the console - copy out of state the worker writes, under the module's own lock if needed. */
    int (*lines)(char (*out)[LS_EXP_LINE], int max);
    /* Optional console arguments after "exp <id> start", e.g. a frequency. NULL if none. argv[0] is the
       first of them. Called only when there are some, before the start is queued. */
    bool (*configure)(int argc, char **argv, char *why, size_t why_len);
    /* Optional OPTIONS rows (ls_options.h) shown on the experiment page. */
    const ls_opt_t *opts; int n_opts;
    /* True for an experiment that does not use the LoRa chip: the framework neither takes the socket
       from MeshCore nor checks who is driving it, and the page has no RADIO button. */
    bool no_radio;
    /* True for one that needs the LR2021 (its HF input, OOK engine, Z-Wave or Wi-SUN engine, or a
       band the SX126x cannot receive). The list and `exp` mark it, and its start() refuses cleanly
       on any other chip with ls_exp_needs_lr2021. */
    bool lr2021_only;
} ls_experiment_t;

/* The refusal text a start() writes into `why` for a chip that is not an LR2021. */
#define LS_EXP_NEEDS_LR2021 "Needs an LR2021 radio, not on this board"

/* The most experiments the list holds. */
#define LS_EXP_MAX 24

/* Add one to the list. The descriptor must outlive the program (a static
   const). A second registration of the same id, or one past LS_EXP_MAX, is
   ignored. Any task. */
void ls_exp_register(const ls_experiment_t *e);

/* Every built-in experiment, one line each, in ls_experiments_builtin.c.
   Safe to call more than once. */
void ls_exp_register_builtin(void);

/* ------------------------------------------------------------- the list -- */

int ls_exp_count(void);
const ls_experiment_t *ls_exp_at(int i);
const ls_experiment_t *ls_exp_find(const char *id);
/* "IDEA", "TRYING", "WORKS". */
const char *ls_exp_maturity_name(ls_exp_maturity_t m);
/* e->lines, with the count clamped to 0..max and every line terminated.
   0 for an experiment without a readout. */
int ls_exp_read_lines(const ls_experiment_t *e, char (*out)[LS_EXP_LINE], int max);

/* ------------------------------------------------------------ running -- */

/* Ask for `e` to run: the worker stops whatever runs, then starts it.
   Returns at once; ls_exp_state_line says how it went. */
void ls_exp_start(const ls_experiment_t *e);
/* Ask for nothing to run. */
void ls_exp_stop(void);
/* The experiment running now, or NULL. */
const ls_experiment_t *ls_exp_running(void);
/* What was last asked to run, or NULL for nothing: what will be running
   once the worker has caught up. */
const ls_experiment_t *ls_exp_wanted(void);
/* True while a start or stop has been asked for and the worker has not
   carried it out yet. */
bool ls_exp_busy(void);
/* Wait up to `timeout_ms` for the worker to carry out what was asked.
   True when it has. Not for the UI task: the console waits, screens draw
   ls_exp_state_line instead. */
bool ls_exp_settle(int timeout_ms);
/* What `e` is doing, for a status line: "running 1m12s", "starting",
   "stopping", "stopped", or why its last start failed. */
void ls_exp_state_line(const ls_experiment_t *e, char *out, size_t n);

/* One pass of the worker: carry out a start or stop that is waiting, then
   poll the running experiment. True while there is something to come back
   for. The worker task calls it; the bench and the simulator call it
   directly. One caller at a time. */
bool ls_exp_service(void);

/* Empty the list and drop every request and failure, without calling
   stop. For a bench test that starts over. */
void ls_exp_forget(void);

/* The `exp` console command, argv[0] being "exp":
     exp                       list: id, maturity, state
     exp <id>                  its live lines and state
     exp <id> start [args...]  configure with args, then start, and wait
     exp <id> stop | exp stop  stop it
   Returns 0 on success, 1 otherwise. */
int ls_exp_console(int argc, char **argv);

/* --------------------------------------------------------- board seam -- */

/* What the framework asks of the board: ls_experiments_hw.c on the device,
   a fake on the bench. */

/* NULL when the LoRa socket can be taken now, else why not ("LoRa Labs
   owns the radio"). Called on the worker before every start. */
const char *ls_exp_hw_radio_busy(void);
/* Take the socket from MeshCore, waiting up to a second for a transmit to
   finish. False when MeshCore would not let go. */
bool ls_exp_hw_radio_take(void);
void ls_exp_hw_radio_give(void);
/* Make sure the worker task exists and have it look at the request now.
   False when there is no worker, and whoever waits services it inline. */
bool ls_exp_hw_wake(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_EXPERIMENTS_H */
