#include "home_widget_pref.h"

static home_widget_id_t s_selected = HOME_WIDGET_RECEIVER;

void home_widget_pref_init(bool read_ok, int stored)
{
    s_selected = read_ok && stored >= 0 && stored < HOME_WIDGET_COUNT
        ? (home_widget_id_t)stored : HOME_WIDGET_RECEIVER;
}

home_widget_id_t home_widget_pref_get(void) { return s_selected; }

bool home_widget_pref_set(int id, bool (*persist)(int))
{
    if (id < 0 || id >= HOME_WIDGET_COUNT || !persist) return false;
    if (id == (int)s_selected) return true;
    if (!persist(id)) return false;
    s_selected = (home_widget_id_t)id;
    return true;
}
