/* LS_TEST_SOURCES: none; it runs the lssim this build made, with -C */
/* Every value and action the firmware registers, counted the way the board
   does it: the built-in sets at boot, then each screen entered once, since
   MAP and P25 register theirs on entry. lssim already links all of that, so
   this runs it instead of linking it a second time.

   2.8.2 passed every other suite with sys.volume refused at boot. */

#include "ls_test.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define popen  _popen
#define pclose _pclose
#endif

LS_CASE(every_value_and_action_the_board_registers_fits_with_room_to_spare)
{
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "%s home -e -C 2>&1", LSSIM_EXE);
#ifdef _WIN32
    /* cmd.exe wants the program's own path with backslashes. */
    for (char *c = cmd; *c && *c != ' '; c++) if (*c == '/') *c = '\\';
#endif
    FILE *p = popen(cmd, "r");
    LS_CHECK_MSG(p != NULL, "could not run %s", cmd);
    if (!p) return;

    char line[256], said[256] = "";
    while (fgets(line, sizeof(line), p)) {
        if (!strstr(line, "lssim:") && !strstr(line, "table full")) continue;
        line[strcspn(line, "\r\n")] = 0;
        ls_note("%s", line);
        snprintf(said, sizeof(said), "%s", line);
    }
    const int rc = pclose(p);
    LS_CHECK_MSG(rc == 0, "lssim -C failed (%d): %s", rc, said);
}
