/* See ls_app.h. The directory over the screen router. */
#include "ls_app.h"

#include <string.h>
#include "esp_attr.h"

#include "ls_anim.h"

#define MAX_APPS LS_TUI_MAX_SCREENS

EXT_RAM_BSS_ATTR static ls_app_t s_app[MAX_APPS];
EXT_RAM_BSS_ATTR static int      s_screen_index[MAX_APPS];
static int      s_count;
static int      s_selected;
static const ls_tui_screen_t *s_retired_screen;
static int s_retired_index;
static bool     s_main_prefix = true;

/* The contract, checked where an app enters the directory rather than
   trusted. A descriptor that cannot say what it is for, what it keeps, or
   what it does with a position is refused - see ls_app_doc_t. */
static bool doc_is_complete(const ls_app_doc_t *d)
{
    if (!d) return false;
    if (!d->purpose || !d->purpose[0]) return false;
    if (d->records != LS_APP_RECORDS_NOTHING &&
        (!d->record_note || !d->record_note[0])) return false;
    if (d->gps != LS_APP_GPS_UNUSED &&
        (!d->gps_note || !d->gps_note[0])) return false;
    return true;
}

const char *ls_app_register_why(int rc)
{
    switch (rc) {
    case LS_APP_REG_BAD_ARGS:  return "no screen to show";
    case LS_APP_REG_NO_DOC:    return "does not say what it is for";
    case LS_APP_REG_DIR_FULL:  return "no room left in the app directory";
    case LS_APP_REG_NO_SCREEN: return "no room left in the screen table";
    default:                   return "refused by the app directory";
    }
}

int ls_app_register(const ls_app_t *app)
{
    if (!app || !app->screen)     return LS_APP_REG_BAD_ARGS;
    if (s_count >= MAX_APPS)      return LS_APP_REG_DIR_FULL;
    if (!doc_is_complete(app->doc)) return LS_APP_REG_NO_DOC;
    int si = app->screen == s_retired_screen ? s_retired_index : -1;
    for (int i = 0; i < s_count; i++)
        if (s_app[i].screen == app->screen) { si = s_screen_index[i]; break; }
    if (si < 0) si = ls_tui_screen_register(app->screen);
    if (si < 0) return LS_APP_REG_NO_SCREEN;
    s_app[s_count] = *app;
    s_screen_index[s_count] = si;
    s_count++;

    if (s_main_prefix && app->cat == LS_APP_MAIN)
        ls_tui_screen_set_tab_count(si + 1);
    else
        s_main_prefix = false;

    return si;
}

bool ls_app_update(const ls_app_t *app)
{
    if (!app || !app->id || !doc_is_complete(app->doc)) return false;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_app[i].id, app->id) == 0 && s_app[i].screen == app->screen) {
            s_app[i] = *app; return true;
        }
    return false;
}

bool ls_app_unregister(const char *id)
{
    if (!id) return false;
    for (int i = 0; i < s_count; i++)
        if (s_app[i].cat == LS_APP_USER && strcmp(s_app[i].id, id) == 0) {
            s_retired_screen = s_app[i].screen;
            s_retired_index = s_screen_index[i];
            memmove(&s_app[i], &s_app[i + 1], (s_count - i - 1) * sizeof(s_app[0]));
            memmove(&s_screen_index[i], &s_screen_index[i + 1], (s_count - i - 1) * sizeof(s_screen_index[0]));
            if (s_selected > i) s_selected--;
            else if (s_selected == i) s_selected = 0;
            s_count--; return true;
        }
    return false;
}

int ls_app_count(void) { return s_count; }

const ls_app_t *ls_app_at(int index)
{
    return (index >= 0 && index < s_count) ? &s_app[index] : NULL;
}

const ls_app_t *ls_app_by_id(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < s_count; i++)
        if (s_app[i].id && strcmp(s_app[i].id, id) == 0) return &s_app[i];
    return NULL;
}

int ls_app_list(ls_app_cat_t cat, const ls_app_t **out, int cap)
{
    int n = 0;
    for (int i = 0; i < s_count && n < cap; i++)
        if (s_app[i].cat == cat) out[n++] = &s_app[i];
    return n;
}

const ls_app_t *ls_app_current(void)
{
    const int cur = ls_tui_screen_current();
    if (s_selected < s_count && s_screen_index[s_selected] == cur) return &s_app[s_selected];
    for (int i = 0; i < s_count; i++)
        if (s_screen_index[i] == cur) return &s_app[i];
    return NULL;
}

void ls_app_open(int index)
{
    if (index < 0 || index >= s_count) return;

    /* The launch animation. This was swapped for a bare cancel in the rc2
       commit, one line inside a 112-file change, which left the whole
       animation path built and wired - the router still draws it and both
       input paths still dismiss it - with nothing anywhere to start it. */
    s_selected = index;
    if (s_app[index].select) s_app[index].select(s_app[index].id);
    ls_anim_start(s_app[index].icon, s_app[index].name, s_app[index].hue);
    ls_tui_screen_show(s_screen_index[index]);
}
