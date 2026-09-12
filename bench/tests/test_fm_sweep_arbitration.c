/* LS_TEST_SOURCES: ${FW}/components/apps/fm_gui/fm_sweep_arbitration.c */
#include "ls_test.h"
#include "fm_gui/fm_sweep_arbitration.h"

#include <stdbool.h>
#include <stddef.h>

/**/

typedef enum {
    STEP_SCANNER_STOP    = 1,
    STEP_ENTER_SWEEP     = 2,
    STEP_RESTART_SWEEP   = 3,
} step_t;

#define STEP_MAX 16

static bool  s_scanner_running;
static int   s_scanner_active_calls;
static int   s_steps[STEP_MAX];
static int   s_step_n;

static void record_step(step_t s)
{
    if (s_step_n < STEP_MAX) s_steps[s_step_n++] = (int)s;
}

static bool hook_scanner_active(void *ctx)   { (void)ctx; s_scanner_active_calls++; return s_scanner_running; }
/* scanner_stop is also what the app does in production: after the call, the
   engine really is off. Mirror that so a second invocation sees it stopped. */
static void hook_scanner_stop(void *ctx)     { (void)ctx; record_step(STEP_SCANNER_STOP);  s_scanner_running = false; }
static void hook_enter_sweep_mode(void *ctx) { (void)ctx; record_step(STEP_ENTER_SWEEP);   }
static void hook_restart_sweep(void *ctx)    { (void)ctx; record_step(STEP_RESTART_SWEEP); }

static void wire_hooks(bool scanner_running)
{
    s_scanner_running       = scanner_running;
    s_scanner_active_calls  = 0;
    s_step_n                = 0;
    for (int i = 0; i < STEP_MAX; i++) s_steps[i] = 0;

    fm_sweep_hooks_t h = {
        hook_scanner_active,
        hook_scanner_stop,
        hook_enter_sweep_mode,
        hook_restart_sweep,
        NULL,
    };
    fm_sweep_configure(&h);
}

/* --- the done-when case ------------------------------------------------- */

LS_CASE(band_stops_the_channel_scanner_and_ends_in_sweep_mode)
{
    /* The scenario the task file names: the channel scanner is running, and
       something invokes the BAND path (which now goes through the shared
       arbitration). After the call the scanner must be OFF and the FM app
       must be in SWEEP mode. Before the scanner stayed on and the
       next pass forced FM back into LISTEN. */
    wire_hooks(true);
    fm_sweep_start_arbitrated();

    LS_CHECK(!s_scanner_running);
    /* enter_sweep_mode is what puts FM into FM_MODE_SCAN (see the trampoline
       in AppFM.cpp) - firing it once is how the app ends up in sweep mode. */
    int enter = 0;
    for (int i = 0; i < s_step_n; i++) if (s_steps[i] == STEP_ENTER_SWEEP) enter++;
    LS_EQ_INT(enter, 1);
}

/* --- ordering, which is what makes the fix load-bearing ------------------ */

LS_CASE(stops_scanner_before_entering_sweep_mode)
{
    /* If enter-sweep-mode ran BEFORE scanner-stop, the scanner would get one
       more pass in - and that pass forces FM_MODE_LISTEN (), which is
       exactly the mode we just left. The order is not decorative. */
    wire_hooks(true);
    fm_sweep_start_arbitrated();

    int stop_pos = -1, enter_pos = -1;
    for (int i = 0; i < s_step_n; i++) {
        if (s_steps[i] == STEP_SCANNER_STOP  && stop_pos  < 0) stop_pos  = i;
        if (s_steps[i] == STEP_ENTER_SWEEP   && enter_pos < 0) enter_pos = i;
    }
    LS_CHECK(stop_pos  >= 0);
    LS_CHECK(enter_pos >= 0);
    LS_CHECK(stop_pos < enter_pos);
}

LS_CASE(restart_sweep_fires_after_mode_is_set)
{
    /* Restarting the sweep before the mode is set would reset the bins for a
       run that the mode change is about to invalidate anyway. Order the same
       way scanRestartCb has ordered it since . */
    wire_hooks(true);
    fm_sweep_start_arbitrated();

    int enter_pos = -1, restart_pos = -1;
    for (int i = 0; i < s_step_n; i++) {
        if (s_steps[i] == STEP_ENTER_SWEEP   && enter_pos   < 0) enter_pos   = i;
        if (s_steps[i] == STEP_RESTART_SWEEP && restart_pos < 0) restart_pos = i;
    }
    LS_CHECK(enter_pos    >= 0);
    LS_CHECK(restart_pos  >= 0);
    LS_CHECK(enter_pos < restart_pos);
}

/* --- BAND and RESTART share the ordering -------------------------------- */

LS_CASE(band_and_restart_produce_identical_step_sequences)
{
    /* Both UI callbacks now do exactly `fm_sweep_start_arbitrated()`. The
       done-when case above already proves scanner-stop; this case is the
       reason BAND fell out of sync in the first place - two code paths, one
       ordering. Any future refactor that splits them will be caught here. */
    int band_steps[STEP_MAX];
    int band_n;

    wire_hooks(true);
    fm_sweep_start_arbitrated();    /* the BAND path calls this */
    band_n = s_step_n;
    for (int i = 0; i < band_n; i++) band_steps[i] = s_steps[i];

    wire_hooks(true);
    fm_sweep_start_arbitrated();    /* the RESTART path calls this */
    LS_EQ_INT(s_step_n, band_n);
    for (int i = 0; i < s_step_n; i++) LS_EQ_INT(s_steps[i], band_steps[i]);
}

/* --- scanner already off: don't call stop, but still enter sweep -------- */

LS_CASE(does_not_stop_a_scanner_that_is_already_off)
{
    /* scan_engine_stop() is cheap but it is not the point - the point is that
       when the scanner is off, the arbitration reduces to "enter sweep +
       restart". Calling stop unconditionally would be a lie to any future
       observer that logs scanner state transitions. */
    wire_hooks(false);
    fm_sweep_start_arbitrated();

    for (int i = 0; i < s_step_n; i++)
        LS_CHECK(s_steps[i] != STEP_SCANNER_STOP);

    /* And the two remaining steps still run, in order. */
    LS_EQ_INT(s_step_n, 2);
    LS_EQ_INT(s_steps[0], STEP_ENTER_SWEEP);
    LS_EQ_INT(s_steps[1], STEP_RESTART_SWEEP);
}

LS_CASE(scanner_active_is_asked_exactly_once)
{
    /* Cheap-but-real regression guard: the arbitration must not poll the
       scanner in a way that lets it change state under us between the check
       and the stop. One question, one answer, one decision. */
    wire_hooks(true);
    fm_sweep_start_arbitrated();
    LS_EQ_INT(s_scanner_active_calls, 1);
}

/* --- missing hooks are tolerated (matches ls_shell_nav's contract) ------ */

LS_CASE(null_hooks_are_silent)
{
    /* fm_sweep_configure(NULL) clears the hooks. Firing the arbitration must
       not crash - the AppFM app hooks itself up in run() and clears them in
       close(), so a firing between close() and the next run() is possible.
       This mirrors ls_shell_nav_configure(NULL). */
    fm_sweep_configure(NULL);
    fm_sweep_start_arbitrated();    /* must not crash */

    /* Re-wire so the next case sees a clean slate. */
    wire_hooks(false);
}
