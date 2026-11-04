/* ls_power.h on the bench: the mode and what WATCH keeps, as state only.
   The board's ls_power.c drives display_ctl, the GPS and MeshCore, which the
   host has as fakes of its own; the screens only need the words and the
   choice to move. */
#include "ls_power.h"

static ls_pwr_mode_t s_mode = LS_PWR_FULL;
static ls_pwr_keep_t s_keep = LS_KEEP_ALL;

static const char *const MODE_NAMES[LS_PWR_MODES] = { "FULL", "SAVER", "WATCH" };
static const char *const MODE_WHAT[LS_PWR_MODES] = {
    "everything at full rate",
    "half light, dim at 30 s",
    "dark at 20 s, one job",
};
static const char *const KEEP_NAMES[LS_KEEPS] = { "EVERYTHING", "MESH", "P25", "GPS LOG" };

void ls_power_init(void) {}
ls_pwr_mode_t ls_power_mode(void) { return s_mode; }
void ls_power_set_mode(ls_pwr_mode_t m) { if (m >= 0 && m < LS_PWR_MODES) s_mode = m; }
ls_pwr_keep_t ls_power_keep(void) { return s_keep; }
void ls_power_set_keep(ls_pwr_keep_t k) { if (k >= 0 && k < LS_KEEPS) s_keep = k; }
const char *ls_power_mode_name(int m) { return m >= 0 && m < LS_PWR_MODES ? MODE_NAMES[m] : "?"; }
const char *ls_power_mode_what(int m) { return m >= 0 && m < LS_PWR_MODES ? MODE_WHAT[m] : ""; }
const char *ls_power_keep_name(int k) { return k >= 0 && k < LS_KEEPS ? KEEP_NAMES[k] : "?"; }
void ls_power_tick(void) {}
int ls_power_frame_ms(int normal_ms) { return normal_ms; }
bool ls_power_dark(void) { return false; }
const char *ls_power_parked(void) { return ""; }
/* LSSIM_POWER_HISTORY=1: an hour of a board idling, then a radio app opened,
   so the graph can be looked at. */
#include <stdlib.h>
int ls_power_history(short *out, int max)
{
    if (!getenv("LSSIM_POWER_HISTORY")) return 0;
    int n = max < LS_POWER_HISTORY ? max : LS_POWER_HISTORY;
    for (int i = 0; i < n; i++) out[i] = (short)(i < 40 ? -310 - (i * 7) % 23 : -520 - (i * 13) % 41);
    return n;
}
void ls_power_console_register(void) {}
