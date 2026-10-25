/* Multiple native tiles share a screen and retain their own identity, including live refresh. */
#include "ls_anim.h"
#include "ls_app.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const ls_tui_screen_t *screens[64];
static int count, current;
static char selected[32];
static void choose(const char *id) { snprintf(selected, sizeof(selected), "%s", id); }
static void draw(tui_surface *s, tui_rect a) {
    (void)s;
    (void)a;
}
static const ls_tui_screen_t home = {.name = "HOME", .draw = draw},
                             shared = {.name = "APP", .draw = draw};
static const ls_app_doc_t doc = {.purpose = "test"};
int ls_tui_screen_register(const ls_tui_screen_t *s) {
    for (int i = 0; i < count; i++)
        if (screens[i] == s)
            return i;
    assert(count < 64);
    screens[count] = s;
    return count++;
}
void ls_tui_screen_set_tab_count(int n) { (void)n; }
int ls_tui_screen_current(void) { return current; }
void ls_tui_screen_show(int i) { current = i; }
void ls_anim_start(int icon, const char *name, uint8_t hue) {
    (void)icon;
    (void)name;
    (void)hue;
}
int main(void) {
    static char id[63][32], name[63][16];
    ls_app_t a = {.id = "home", .name = "HOME", .cat = LS_APP_MAIN, .screen = &home, .doc = &doc};
    assert(ls_app_register(&a) == 0);
    for (int i = 0; i < 24; i++) {
        snprintf(id[i], 32, "probe%d", i);
        snprintf(name[i], 16, "PROBE%d", i);
        a = (ls_app_t){.id = id[i],
                       .name = name[i],
                       .cat = LS_APP_USER,
                       .screen = &shared,
                       .doc = &doc,
                       .select = choose};
        assert(ls_app_register(&a) == 1);
    }
    assert(count == 2 && ls_app_count() == 25);
    for (int i = 1; i <= 24; i++) {
        ls_app_open(i);
        assert(!strcmp(selected, id[i - 1]));
        assert(ls_app_current() == ls_app_at(i));
    }
    a = *ls_app_by_id("probe3");
    a.hue = 6;
    a.icon = 11;
    assert(ls_app_update(&a));
    assert(ls_app_by_id("probe3")->hue == 6);
    for (int i = 0; i < 24; i++)
        assert(ls_app_unregister(id[i]));
    assert(ls_app_count() == 1);
    a = (ls_app_t){.id = "again",
                   .name = "AGAIN",
                   .cat = LS_APP_USER,
                   .screen = &shared,
                   .doc = &doc,
                   .select = choose};
    assert(ls_app_register(&a) == 1 && count == 2);
    for (int i = 0; i < 62; i++) {
        snprintf(id[i], 32, "native%d", i);
        a.id = id[i];
        assert(ls_app_register(&a) == 1);
    }
    assert(ls_app_count() == 64 && ls_app_register(&a) == LS_APP_REG_DIR_FULL);
    assert(!ls_app_unregister("home"));
    puts("test_lap_registry: 5 cases passed (24 shared tiles, identity, refresh, reuse, capacity)");
    return 0;
}
