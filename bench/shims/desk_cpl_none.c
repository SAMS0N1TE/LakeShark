/* test_desk_wm links the shell without the Control Panel applets (ls_desk_cplsys.c)
   or the board's display and settings stores: these stand in for them. */

#include <stdbool.h>
#include <stddef.h>

typedef struct dk_sheet dk_sheet_t;

const dk_sheet_t *dk_cpl_sheet(int applet) { (void)applet; return NULL; }

static int  s_user = 80, s_dim_s = 60;
static bool s_dim, s_day;

void display_ctl_set_user(int pct) { s_user = pct; }
int  display_ctl_get_user(void) { return s_user; }
void display_ctl_set_autodim(bool enabled) { s_dim = enabled; }
bool display_ctl_autodim_enabled(void) { return s_dim; }
void display_ctl_set_autodim_timeout(int seconds) { s_dim_s = seconds; }
int  display_ctl_autodim_timeout(void) { return s_dim_s; }
void settings_set_daylight(bool on) { s_day = on; }
void ls_tui_set_daylight(bool on) { s_day = on; }
