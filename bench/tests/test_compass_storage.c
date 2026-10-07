#include "ls_test.h"
#include "../../components/apps/tui/screens/ext/scr_compass.c"

LS_CASE(compass_option_rows_are_read_only)
{
    /* A writable row moves the whole table into scarce internal RAM. */
    LS_CHECK(_Generic(&OPT_COMPASS[0], const ls_opt_t *: true, default: false));
}
