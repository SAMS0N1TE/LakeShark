/* LS_TEST_SOURCES: none; it runs the lssim this build made, with -a */
/* "store.open": an SD app sends people to another app's STORE card
   (MESHTASTIC's MORE LANGUAGES to MESH LANGUAGES). Called as an app calls
   it, READ granted only: STORE comes up on the next frame with that card
   picked, or with the list as it was when the id is not in the catalog. */

#include "ls_test.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define popen  _popen
#define pclose _pclose
#endif

/* Runs lssim on HOME with the action; true when every want[] line shows. */
static bool run(const char *arg, const char *const *want, int n, const char *const *never, int nn)
{
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "%s home -a \"store.open %s\" -f 10 -d -o %s 2>&1", LSSIM_EXE, arg, LSSIM_BMP);
#ifdef _WIN32
    for (char *c = cmd; *c && *c != ' '; c++) if (*c == '/') *c = '\\';
#endif
    FILE *p = popen(cmd, "r");
    LS_CHECK_MSG(p != NULL, "could not run %s", cmd);
    if (!p) return false;
    bool seen[8] = {false}, bad = false;
    char line[512];
    while (fgets(line, sizeof(line), p)) {
        for (int i = 0; i < n && i < 8; i++) if (strstr(line, want[i])) seen[i] = true;
        for (int i = 0; i < nn; i++) if (strstr(line, never[i])) { bad = true; ls_note("unexpected: %s", line); }
    }
    pclose(p);
    bool all = !bad;
    for (int i = 0; i < n && i < 8; i++) {
        if (!seen[i]) ls_note("missing: %s", want[i]);
        all = all && seen[i];
    }
    return all;
}

LS_CASE(store_open_shows_store_at_the_apps_card)
{
    static const char *const want[] = { "lssim: action store.open -> 0 STORE", "LAKESHARK  STORE", "+-IRIDIUM BURSTS" };
    LS_CHECK_MSG(run("iridium", want, 3, NULL, 0), "STORE did not open at IRIDIUM's card");
}

LS_CASE(meshtastic_more_languages_lands_on_mesh_languages)
{
    static const char *const want[] = { "lssim: action store.open -> 0 STORE", "LAKESHARK  STORE", "+-MESH LANGUAGES" };
    LS_CHECK_MSG(run("meshlang", want, 3, NULL, 0), "STORE did not open at MESH LANGUAGES' card");
}

LS_CASE(store_open_with_an_unknown_id_shows_the_list)
{
    static const char *const want[] = { "lssim: action store.open -> 0 STORE", "LAKESHARK  STORE" };
    static const char *const never[] = { "+-IRIDIUM BURSTS" };
    LS_CHECK_MSG(run("no_such_app", want, 2, never, 1), "STORE did not open on its list");
}
