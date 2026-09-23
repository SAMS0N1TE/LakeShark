/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_userapp.c
 *                  ${FW}/components/apps/tui/ls_app.c
 *
 * User apps, loaded the way the board loads them: the real loader, real
 * files, a real directory, and registration asserted rather than parsing
 * alone. */

#include "ls_test.h"

#include "ls_app.h"
#include "ls_userapp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The screen router and the drawing primitives, faked. The loader and the
   app directory are the real ones. Of these, only ls_tui_screen_register()
   is reached from the registration path. */

#include "ls_tui_screen.h"
#include "ls_tui_ui.h"
#include "ls_value.h"
#include "ls_waterfall.h"
#include "ls_anim.h"
#include "tui_core.h"

static const ls_tui_screen_t *s_registered[LS_TUI_MAX_SCREENS];
static int s_registered_n;

int ls_tui_screen_register(const ls_tui_screen_t *screen)
{
    if (!screen || s_registered_n >= LS_TUI_MAX_SCREENS) return -1;
    s_registered[s_registered_n] = screen;
    return s_registered_n++;
}

void ls_tui_screen_set_tab_count(int n) { (void)n; }
int  ls_tui_screen_current(void) { return 0; }
void ls_tui_screen_show(int index) { (void)index; }
void ls_anim_start(int icon, const char *name, uint8_t hue)
{ (void)icon; (void)name; (void)hue; }

void ls_tui_split(tui_rect area, tui_rect *first, tui_rect *second)
{ if (first) *first = area; if (second) *second = area; }
void ls_panel_box(tui_surface *sf, tui_rect r, const char *title, uint8_t hue)
{ (void)sf; (void)r; (void)title; (void)hue; }
void ls_kv(tui_surface *sf, tui_rect r, int row, const char *label,
           const char *value, uint8_t vattr)
{ (void)sf; (void)r; (void)row; (void)label; (void)value; (void)vattr; }
void ls_bar(tui_surface *sf, tui_rect r, int row, int x, int w, float v)
{ (void)sf; (void)r; (void)row; (void)x; (void)w; (void)v; }
void ls_wf_draw_mini(tui_surface *sf, tui_rect area) { (void)sf; (void)area; }
bool ls_value_read(const char *path, ls_val_t *out, const char **unit)
{ (void)path; (void)out; (void)unit; return false; }
void tui_put_str(tui_surface *s, tui_rect clip, int x, int y, const char *str,
                 uint8_t attr)
{ (void)s; (void)clip; (void)x; (void)y; (void)str; (void)attr; }
tui_rect tui_rect_make(int x, int y, int w, int h)
{ tui_rect r = { x, y, w, h }; return r; }

static char s_dir[128];

static void dir_make(void)
{
    ls_test_fixture_dir(s_dir, sizeof(s_dir), "ls_userapp_test");
    ls_test_mkdir(s_dir);
}

static void dir_clear(void)
{
    /* The files this suite writes, in the directory it made. */
    static const char *const junk[] = {
        "one.lsapp", "two.lsapp", "bad.lsapp", "notes.txt",
        "a0.lsapp", "a1.lsapp", "a2.lsapp", "a3.lsapp",
        "a4.lsapp", "a5.lsapp", "a6.lsapp", "a7.lsapp",
        "a8.lsapp", "a9.lsapp",
    };
    char p[256];
    for (unsigned i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", s_dir, junk[i]);
        unlink(p);
    }
}

static void write_file(const char *name, const char *text)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", s_dir, name);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static const char APP_ONE[] =
    "version=1\n"
    "name=meter\n"
    "sub=bench\n"
    "icon=wave\n"
    "color=cyan\n"
    "panel=SIGNAL\n"
    "value=RSSI|rf.rssi\n";

static const char APP_TWO[] =
    "version=1\n"
    "name=probe\n"
    "panel=TEMP\n"
    "bar=LEVEL|env.level\n";

static int s_loaded = -1;

static void card_teardown(void)
{
    dir_clear();
    ls_test_rmdir(s_dir);
}

/* The loader does one boot-time pass and does not de-duplicate, so the card
   is set up once per run and every case reads that one result. Case order is
   not fixed - constructors run in source order under glibc and in reverse
   under mingw - so any case that reads the card calls this first. */
static void card_once(void)
{
    if (s_loaded >= 0) return;
    dir_make();
    dir_clear();
    write_file("one.lsapp", APP_ONE);
    write_file("two.lsapp", APP_TWO);
    write_file("notes.txt", "not an app\n");   /* the loader skips it */
    s_loaded = ls_userapp_load_dir(s_dir);
    atexit(card_teardown);
}

LS_CASE(the_apps_on_the_card_actually_register)
{
    card_once();

    LS_EQ_INT(s_loaded, 2);
    LS_EQ_INT(ls_app_count(), 2);

    const ls_app_t *a = ls_app_by_id("meter");
    LS_CHECK_MSG(a != NULL, "the loaded app is not in the directory");
    if (!a) return;
    LS_CHECK(a->cat == LS_APP_USER);
    LS_CHECK_MSG(ls_userapp_last_error() == NULL,
                 "a good card reported an error");
}

LS_CASE(a_loaded_app_carries_a_complete_doc)
{
    card_once();
    const ls_app_t *a = ls_app_by_id("meter");
    LS_CHECK_MSG(a != NULL, "the loaded app is not in the directory");
    if (!a) return;
    LS_CHECK_MSG(a->doc != NULL, "a user app registered without a doc");
    if (!a->doc) return;
    LS_CHECK(a->doc->purpose && a->doc->purpose[0]);
    /* The purpose names the file the app came from. */
    LS_CHECK_MSG(strstr(a->doc->purpose, "one.lsapp") != NULL,
                 a->doc->purpose);
    /* A described screen reads published values, so it keeps nothing, and
       an app that keeps nothing carries no record note. */
    LS_CHECK(a->doc->records == LS_APP_RECORDS_NOTHING);
    LS_CHECK(a->doc->record_note == NULL);
    LS_CHECK(a->doc->gps == LS_APP_GPS_UNUSED);
    LS_CHECK(a->doc->gps_note == NULL);
}

LS_CASE(each_app_keeps_its_own_purpose_line)
{
    card_once();
    const ls_app_t *one = ls_app_by_id("meter");
    const ls_app_t *two = ls_app_by_id("probe");
    LS_CHECK_MSG(one && two, "both apps should be in the directory");
    if (!one || !two || !one->doc || !two->doc) return;
    LS_CHECK_MSG(one->doc != two->doc, "two apps share one doc");
    LS_CHECK_MSG(strstr(two->doc->purpose, "two.lsapp") != NULL,
                 two->doc->purpose);
    LS_CHECK_MSG(strstr(one->doc->purpose, "one.lsapp") != NULL,
                 one->doc->purpose);
}

LS_CASE(a_refusal_says_which_wall_it_hit)
{
    /* Each refusal reads differently on the home screen. */
    LS_CHECK(strcmp(ls_app_register_why(LS_APP_REG_NO_DOC),
                    "no room left in the app directory") != 0);
    LS_CHECK(strstr(ls_app_register_why(LS_APP_REG_DIR_FULL), "room") != NULL);
    LS_CHECK(ls_app_register_why(LS_APP_REG_BAD_ARGS)[0] != 0);
    LS_CHECK(ls_app_register_why(0)[0] != 0);
}

LS_CASE(an_undocumented_descriptor_is_still_refused)
{
    /* The loader meets the contract; it is not exempt from it. */
    static const ls_tui_screen_t scr = { .name = "NODOC" };
    ls_app_t app = {
        .id = "nodoc", .name = "NODOC", .cat = LS_APP_USER,
        .screen = &scr,
    };
    LS_EQ_INT(ls_app_register(&app), LS_APP_REG_NO_DOC);

    /* Claims to keep something without saying what. */
    static const ls_app_doc_t half = {
        .purpose = "Keeps something without saying what.",
        .records = LS_APP_RECORDS_MANUAL,
    };
    app.doc = &half;
    LS_EQ_INT(ls_app_register(&app), LS_APP_REG_NO_DOC);

    app.screen = NULL;
    LS_EQ_INT(ls_app_register(&app), LS_APP_REG_BAD_ARGS);
}

LS_CASE(a_malformed_file_is_refused_with_its_line)
{
    /* Its own directory, so the one-shot loader reads it alone. */
    char dir[160];
    ls_test_fixture_dir(dir, sizeof(dir), "ls_userapp_bad");
    ls_test_mkdir(dir);
    char p[256];
    snprintf(p, sizeof(p), "%s/bad.lsapp", dir);
    FILE *f = fopen(p, "w");
    LS_CHECK_MSG(f != NULL, "could not write the fixture");
    if (f) { fputs("version=1\nname=broken\nnonsense\n", f); fclose(f); }

    LS_EQ_INT(ls_userapp_load_dir(dir), 0);
    const char *e = ls_userapp_last_error();
    LS_CHECK_MSG(e != NULL, "a malformed app reported no error");
    if (e) {
        LS_CHECK_MSG(strstr(e, "bad.lsapp") != NULL, e);
        LS_CHECK_MSG(strstr(e, "line 3") != NULL, e);
    }
    LS_CHECK_MSG(ls_app_by_id("broken") == NULL,
                 "a malformed app reached the directory");
    unlink(p);
    ls_test_rmdir(dir);
}

LS_CASE(a_missing_card_is_not_an_error)
{
    LS_EQ_INT(ls_userapp_load_dir("ls_userapp_no_such_dir"), 0);
    LS_CHECK(ls_userapp_last_error() == NULL);
    LS_EQ_INT(ls_userapp_load_dir(NULL), 0);
}
