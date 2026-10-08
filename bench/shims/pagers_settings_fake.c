#include "settings.h"
static settings_pagers_t cfg = { .hz = 929612500, .dwell = 6, .probe = -1 };
void settings_get_pagers(settings_pagers_t *out) { if (out) *out = cfg; }
bool settings_set_pagers(const settings_pagers_t *in) { cfg = *in; return true; }
