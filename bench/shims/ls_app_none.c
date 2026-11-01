/* No app table in the FILES tests: no app to hand a file to. */
#include "ls_app.h"

const ls_app_t *ls_app_by_id(const char *id) { (void)id; return NULL; }
