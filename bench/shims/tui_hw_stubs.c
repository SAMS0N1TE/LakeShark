/* The hardware a screen touches, stood down for the host.

   Everything here is a leaf the layout tests do not exercise and cannot have:
   a fuel gauge on I2C, and the receiver plumbing that decides which radio is
   feeding the spectrum. The drawing code above them is linked for real, which
   is the point - a faked ls_tui_split once disagreed with the real one and
   hid a portrait layout bug for a week, so the rule here is that only the
   parts with a wire attached get replaced.

   These are not lies about behaviour, either. A board with no gauge and no
   receiver running is a state the firmware genuinely has, and it is the state
   these answers describe: the chrome draws no battery and gets the full width
   back, and a waterfall with nothing pushed into it reports itself empty and
   the screens draw their "waiting for the receiver" message. A test that
   wants a drawn waterfall pushes rows itself through ls_wf_push. */
#include <stdbool.h>
#include <stddef.h>

#include "ls_gauge.h"
#include "ls_wf_source.h"
#include "ls_userapp.h"

/* ---- fuel gauge ---------------------------------------------------- */

bool ls_gauge_present(void) { return false; }

bool ls_gauge_get(ls_gauge_t *out)
{
    if (out) {
        const ls_gauge_t empty = { 0 };
        *out = empty;
    }
    return false;
}

/* ---- what feeds the waterfall --------------------------------------- */

/* Stubbed for the tests, real for the simulator. */

#ifndef LS_WF_SOURCE_REAL

static ls_wf_src_t s_src;

void        ls_wf_source_select(ls_wf_src_t src) { s_src = src; }
ls_wf_src_t ls_wf_source_get(void)               { return s_src; }
const char *ls_wf_source_name(void)              { return "none"; }
void        ls_wf_source_pump(void)              { }
void        ls_wf_source_release(void)           { }
/* FM's SWEEP page starts the sweep through this, so the screen tests
   need it. Selecting is all a stub can honestly do: there is no receiver. */
bool        ls_wf_source_start(ls_wf_src_t src)  { s_src = src; return true; }

#endif

/* ---- the receiver a screen asks for ---------------------------------- */

/* The router asks for a receiver on every screen change. There is no
   receiver on a desktop and no radio app registry either, so this records
   nothing and does nothing - the question under test is whether the router
   ASKS, and a test that wants to check that asks the router, not this. */
void ls_tui_radio_want(const char *mode_name) { (void)mode_name; }
/* The bench has no receiver, so nothing is ever claimed. A screen
   that lights its lamp off this would light it never here, which is the
   right way for a stub to be wrong. */
const char *ls_tui_radio_claimed(void) { return 0; }

/* ---- apps loaded from the card --------------------------------------- */

int ls_userapp_load_dir(const char *dir) { (void)dir; return 0; }

const char *ls_userapp_last_error(void) { return NULL; }

/* ---- the card, the build and the last crash -------------------------- */

#include "ls_sdcard.h"
#include "ls_crash.h"
#include "ls_version.h"

esp_err_t ls_sdcard_mount(void)   { return ESP_ERR_NOT_SUPPORTED; }
void      ls_sdcard_unmount(void) { }
bool      ls_sdcard_mounted(void) { return false; }

bool ls_sdcard_size(uint64_t *total, uint64_t *free_bytes)
{
    if (total) *total = 0;
    if (free_bytes) *free_bytes = 0;
    return false;
}

const char *ls_sdcard_name(void) { return NULL; }
void        ls_sdcard_diagnostics(void) { }

bool ls_crash_present(void) { return false; }

/* ls_version_format and ls_version_is_dirty are pure and linked for real
   where a test wants them; only the part that reads the running image is
   stood down. */
void ls_version_get(ls_version_info_t *v)
{
    if (!v) return;
    v->version = "host";
    v->board = "bench";
    v->idf = "none";
    v->date = __DATE__;
    v->time = __TIME__;
}

/* ---- how this boot started ------------------------------------------- */

/* The health page reports the reset cause, which the device reads
   once at startup and keeps. ls_safe_mode.c itself is linked for real - it
   is pure - so only the part that consulted the chip is stood down here.

   A clean power-on, because that is what a host process did. The interesting
   branch is a panic, and a stub claiming one would put the page permanently
   in its alarming state and hide the ordinary one. */
#include "ls_safe_mode.h"

const ls_safe_boot_t *ls_safe_boot_result(void)
{
    static ls_safe_boot_t boot;
    boot.reset = LS_SAFE_RESET_POWERON;
    boot.reset_class = LS_SAFE_CLASS_CLEAN;
    boot.safe = false;
    return &boot;
}
