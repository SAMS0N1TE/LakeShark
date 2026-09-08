/*LS-767*/
/* See fm_sweep_arbitration.h for the why. AppFM installs the four hooks in
   run(); the bench drives them with a counting harness so the ORDER (scanner
   stop -> enter sweep -> restart sweep) is proved rather than argued about. */

#include "fm_sweep_arbitration.h"

#include <string.h>

static fm_sweep_hooks_t s_hooks;

void fm_sweep_configure(const fm_sweep_hooks_t *hooks)
{
    if (hooks) {
        s_hooks = *hooks;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
    }
}

void fm_sweep_start_arbitrated(void)
{
    /* Order matters. See the header comment - stopping the scanner AFTER
       set-mode would let one final scanner pass reset FM_MODE_LISTEN on top of
       the sweep we just asked for. */
    if (s_hooks.scanner_active && s_hooks.scanner_active(s_hooks.ctx)) {
        if (s_hooks.scanner_stop) s_hooks.scanner_stop(s_hooks.ctx);
    }
    if (s_hooks.enter_sweep_mode) s_hooks.enter_sweep_mode(s_hooks.ctx);
    if (s_hooks.restart_sweep)    s_hooks.restart_sweep(s_hooks.ctx);
}
