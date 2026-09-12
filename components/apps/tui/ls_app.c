/* See ls_app.h. The directory over the screen router. */
#include "ls_app.h"

#include <string.h>

#include "ls_anim.h"

#define MAX_APPS LS_TUI_MAX_SCREENS

static ls_app_t s_app[MAX_APPS];
static int      s_screen_index[MAX_APPS];
static int      s_count;
static bool     s_main_prefix = true;

int ls_app_register(const ls_app_t *app)
{
    if (!app || !app->screen || s_count >= MAX_APPS) return -1;
    const int si = ls_tui_screen_register(app->screen);
    if (si < 0) return -1;
    s_app[s_count] = *app;
    s_screen_index[s_count] = si;
    s_count++;

    if (s_main_prefix && app->cat == LS_APP_MAIN)
        ls_tui_screen_set_tab_count(si + 1);
    else
        s_main_prefix = false;

    return si;
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
    for (int i = 0; i < s_count; i++)
        if (s_screen_index[i] == cur) return &s_app[i];
    return NULL;
}

void ls_app_open(int index)
{
    if (index < 0 || index >= s_count) return;

    ls_anim_start(s_app[index].icon, s_app[index].name, s_app[index].hue);
    ls_tui_screen_show(s_screen_index[index]);
}
