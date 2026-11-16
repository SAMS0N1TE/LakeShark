#include "ls_test.h"
#include "ls_wireless.h"
#include "ls_tui_screen.h"
#include "ls_keyboard.h"
#include "ls_options.h"
#include "ls_app.h"
#include "ls_rterm.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const ls_tui_screen_t ls_scr_wireless;
static const ls_opt_ctx_t *opened_options;   /* the list the screen opened last */
static bool opt_row(const char *label);
static ls_wireless_snapshot_t model;
static ls_wireless_op_t operation;
static char joined[33], password[65];
static bool active, wide;
static tui_cell back[128 * 72], front[128 * 72];
static tui_surface surface;
static int width, height;

void ls_wireless_get(ls_wireless_snapshot_t *out) { *out = model; }
void ls_wireless_set_active(bool value) { active = value; }
bool ls_wireless_request(ls_wireless_op_t op, const char *ssid, const char *pass)
{
    operation = op;
    snprintf(joined, sizeof(joined), "%s", ssid ? ssid : "");
    snprintf(password, sizeof(password), "%s", pass ? pass : "");
    return true;
}
bool ls_tui_is_wide(void) { return wide; }
/* V opens WIFI SURVEY when it is on the card: the apps the screen can find. */
static ls_app_t card_apps[2] = {{.id = "home"}, {.id = "wifi_survey"}};
static int card_app_count, opened_app = -1;
int ls_app_count(void) { return card_app_count; }
const ls_app_t *ls_app_at(int index) { return index >= 0 && index < card_app_count ? &card_apps[index] : NULL; }
void ls_app_open(int index) { opened_app = index; }
static bool keyboard;
bool ls_tui_keyboard_mode(void) { return keyboard; }
void ls_tui_split(tui_rect a, tui_rect *first, tui_rect *second)
{
    if (wide) {
        *first = tui_rect_make(a.x, a.y, a.w / 2, a.h);
        *second = tui_rect_make(a.x + a.w / 2, a.y, a.w - a.w / 2, a.h);
    } else {
        *first = tui_rect_make(a.x, a.y, a.w, a.h / 2);
        *second = tui_rect_make(a.x, a.y + a.h / 2, a.w, a.h - a.h / 2);
    }
}
static void draw(void)
{
    tui_surface_setup(&surface, back, front, width, height);
    tui_frame_begin(&surface);
    ls_scr_wireless.draw(&surface, tui_rect_make(0, 0, width, height));
}
static bool tap(const char *text)
{
    /* Tabs carry their key inline while a keyboard is attached; the raised
       bar's keys wear theirs on a corner badge, so its labels are bare. */
    const bool bottom_button = text[0] == '[' && text[1] != 'B' && text[1] != 'W';
    if ((!keyboard || bottom_button) && text[0] == '[' && strlen(text) > 4 && text[2] == ']') text += 4;
    for (int row_index = 0; row_index < height; row_index++) {
        int y = bottom_button ? height - 1 - row_index : row_index;
        char row[129];
        for (int x = 0; x < width; x++) row[x] = back[y * width + x].ch;
        row[width] = 0;
        char *p = strstr(row, text);
        if (p) { ls_scr_wireless.touch((int)(p - row) + (int)strlen(text) / 2, y); return true; }
    }
    return false;
}
static void setup(bool landscape)
{
    wide = landscape;
    width = wide ? 123 : 56;
    height = wide ? 24 : 64;
    memset(&model, 0, sizeof(model));
    model.wifi_available = model.bt_available = true;
    model.scan_revision = 1;
    model.ap_count = 16;
    for (int i = 0; i < 16; i++) {
        snprintf(model.aps[i].ssid, sizeof(model.aps[i].ssid), "Network%02d", i);
        model.aps[i].rssi = -40 - i;
        model.aps[i].channel = i % 11 + 1;
        model.aps[i].secure = true;
    }
    ls_scr_wireless.enter();
    ls_scr_wireless.key(LS_TK_CHAR, 'w');
    operation = LS_WIRELESS_NONE;
    draw();
}

LS_CASE(wifi_and_bluetooth_buttons_work_in_both_orientations)
{
    for (int mode = 0; mode < 3; mode++) {
        const int landscape = mode > 0;
        keyboard = mode == 2;
        setup(landscape);
        LS_CHECK(active);
        LS_CHECK(tap("[S] SCAN")); LS_EQ_INT(operation, LS_WIRELESS_SCAN);
        /* AUTO is the CONNECT SAVED row of OPTIONS now. */
        opened_options = NULL;
        LS_CHECK(tap("[O] OPTIONS")); LS_CHECK(opened_options && !strcmp(opened_options->name, "WI-FI"));
        LS_CHECK(opt_row("CONNECT SAVED")); LS_EQ_INT(operation, LS_WIRELESS_SAVED);
        LS_CHECK(tap("[D] LEAVE")); LS_EQ_INT(operation, LS_WIRELESS_LEAVE);
        operation = LS_WIRELESS_NONE;
        ls_scr_wireless.key(LS_TK_CHAR, 'f'); LS_EQ_INT(operation, LS_WIRELESS_NONE);
        LS_CHECK(tap("[B] BLUETOOTH") || tap("[B] BT")); draw();   /* BT when five tabs are narrow */
        LS_CHECK(tap("[S] SCAN")); LS_EQ_INT(operation, LS_WIRELESS_BT_RESCAN);
        LS_CHECK(tap("[D] STOP")); LS_EQ_INT(operation, LS_WIRELESS_BT_STOP);
        ls_scr_wireless.leave(); LS_CHECK(!active);
    }
    keyboard = false;
}

LS_CASE(paged_network_tap_joins_the_name_that_was_drawn)
{
    for (int landscape = 0; landscape < 2; landscape++) {
        setup(landscape);
        LS_CHECK(tap("NEXT")); draw();
        char expected[33] = "";
        for (int i = 1; i < 16; i++) {
            char name[33]; snprintf(name, sizeof(name), "Network%02d", i);
            if (tap(name)) { snprintf(expected, sizeof(expected), "%s", name); break; }
        }
        LS_CHECK(expected[0]);
        LS_CHECK(ls_keyboard_active());
        const char *pass = "valid[]Pass!";
        for (const char *p = pass; *p; p++) ls_keyboard_key(LS_TK_CHAR, *p);
        ls_keyboard_key(LS_TK_ENTER, 0);
        LS_EQ_INT(operation, LS_WIRELESS_JOIN);
        LS_CHECK(!strcmp(joined, expected));
        LS_CHECK(!strcmp(password, pass));
    }
}

LS_CASE(manual_ssid_password_and_cancel_use_the_shared_keyboard)
{
    setup(false);
    ls_scr_wireless.key(LS_TK_CHAR, 'o'); LS_CHECK(opt_row("HIDDEN NETWORK"));
    const char *ssid = "HiddenNet";
    for (const char *p = ssid; *p; p++) ls_keyboard_key(LS_TK_CHAR, *p);
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_CHECK(ls_keyboard_active());
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_EQ_INT(operation, LS_WIRELESS_JOIN);
    LS_CHECK(!strcmp(joined, ssid)); LS_CHECK(!password[0]);
    operation = LS_WIRELESS_NONE;
    ls_scr_wireless.key(LS_TK_CHAR, 'o'); LS_CHECK(opt_row("HIDDEN NETWORK"));
    ls_keyboard_key(LS_TK_ESC, 0);
    LS_EQ_INT(operation, LS_WIRELESS_NONE);
}

static void type_field(const char *text)
{
    while (ls_keyboard_text() && ls_keyboard_text()[0]) ls_keyboard_key(LS_TK_BACKSPACE, 0);
    for (const char *p = text; *p; p++) ls_keyboard_key(LS_TK_CHAR, *p);
    ls_keyboard_key(LS_TK_ENTER, 0);
}

LS_CASE(remote_ssh_buttons_port_learn_and_forget)
{
    setup(false);
    ls_scr_wireless.key(LS_TK_CHAR, 'e'); draw();
    ls_rterm_status_t st;
    /* 'o' is OPTIONS, as everywhere; the web screen is M now. */
    opened_options = NULL;
    ls_scr_wireless.key(LS_TK_CHAR, 'o');
    LS_CHECK(!ls_keyboard_active());
    LS_CHECK(opened_options && !strcmp(opened_options->name, "REMOTE"));
    LS_CHECK(opt_row("SSH PORT"));
    LS_CHECK(ls_keyboard_active());
    type_field("2323");                         /* telnet's: refused */
    ls_rterm_status(&st); LS_EQ_INT(st.ssh_port, 22);
    ls_scr_wireless.key(LS_TK_CHAR, 'r');
    LS_CHECK(ls_keyboard_active());
    type_field("2222");
    ls_rterm_status(&st); LS_EQ_INT(st.ssh_port, 2222);
    draw();
    LS_CHECK(opt_row("LEARN SSH KEYS"));
    ls_rterm_status(&st); LS_CHECK(st.ssh_learn_s > 0);
    draw();
    LS_CHECK(opt_row("LEARN SSH KEYS"));          /* the same row stops it */
    ls_rterm_status(&st); LS_EQ_INT(st.ssh_learn_s, 0);
    LS_CHECK(st.ssh_keys > 0);
    LS_CHECK(opt_row("FORGET SSH KEYS"));
    type_field("no");
    ls_rterm_status(&st); LS_CHECK(st.ssh_keys > 0);
    LS_CHECK(opt_row("FORGET SSH KEYS"));
    type_field("Yes");
    ls_rterm_status(&st); LS_EQ_INT(st.ssh_keys, 0);
    ls_scr_wireless.leave();
}

LS_CASE(unchanged_snapshot_draws_identical_cells)
{
    setup(false);
    static tui_cell copy[128 * 72];
    memcpy(copy, back, sizeof(copy));
    draw();
    LS_CHECK(!memcmp(copy, back, width * height * sizeof(back[0])));
}

LS_CASE(history_samples_once_per_second_and_breaks_for_scans)
{
    ls_wireless_history_t h = {0};
    ls_wireless_history_push(&h, 1000, -60, true, -50, true, true, 10, 20);
    ls_wireless_history_push(&h, 1500, -40, true, -30, true, true, 30, 40);
    LS_EQ_INT(h.count, 1);
    ls_wireless_history_push(&h, 2000, -61, true, -51, true, true, 12, 25);
    LS_EQ_INT(h.samples[1].rx, 2); LS_EQ_INT(h.samples[1].tx, 5);
    ls_wireless_history_push(&h, 7000, -61, true, -51, true, true, 50, 90);
    LS_EQ_INT(h.count, 7);
    LS_EQ_INT(h.samples[2].valid, 0);
    LS_CHECK(!(h.samples[6].valid & LS_WIRELESS_RATE_VALID));
}

LS_CASE(history_handles_counter_wrap_and_reconnect)
{
    ls_wireless_history_t h = {0};
    ls_wireless_history_push(&h, 1000, 0, false, -50, true, true, UINT32_MAX - 1, 20);
    ls_wireless_history_push(&h, 2000, 0, false, -50, true, true, 1, 25);
    LS_EQ_INT(h.samples[1].rx, 3);
    LS_CHECK(!(h.samples[1].valid & LS_WIRELESS_WIFI_VALID));
    ls_wireless_history_push(&h, 3000, 0, false, 0, false, false, 0, 0);
    ls_wireless_history_push(&h, 4000, 0, false, -50, true, true, 1, 1);
    LS_CHECK(!(h.samples[3].valid & LS_WIRELESS_RATE_VALID));
}

static const ls_opt_ctx_t *opened_options;
void ls_opt_open(const ls_opt_ctx_t *ctx) { opened_options = ctx; }
ls_btn_t ls_opt_button(const ls_opt_ctx_t *ctx) { return (ls_btn_t){ "OPTIONS", ctx ? ctx->name : NULL, LS_OPT_KEY, false, false }; }
/* Run a row of the list the screen opened last, the way the real list would. */
static bool opt_row(const char *label)
{
    if (!opened_options) return false;
    for (int i = 0; i < opened_options->n; i++) {
        const ls_opt_t *o = &opened_options->opt[i];
        if (strcmp(o->label, label)) continue;
        if (o->why_not && o->why_not(o)) return false;
        if (o->act) o->act(o);
        return true;
    }
    return false;
}

void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch)
{
    if (cols) *cols = width;
    if (rows) *rows = height;
    if (cw) *cw = 10;
    if (ch) *ch = 17;
}

static bool on_screen(const char *text)
{
    for (int y = 0; y < height; y++) {
        char row[129];
        for (int x = 0; x < width; x++) row[x] = back[y * width + x].ch;
        row[width] = 0;
        if (strstr(row, text)) return true;
    }
    return false;
}

LS_CASE(saved_network_selection_forgets_one_in_both_orientations)
{
    for (int landscape = 0; landscape < 2; ++landscape) {
        setup(landscape);
        model.saved_count = 8;
        for (int i = 0; i < 8; ++i) snprintf(model.saved[i], 33, "Saved%02d", i);
        model.wifi_connected = true;
        strcpy(model.ssid, "Saved00");
        model.aps[0].saved = true;
        draw();
        LS_CHECK(on_screen("SAVED / 8"));
        LS_CHECK(on_screen("* Saved00"));
        LS_CHECK(on_screen("PASSWORD / SAVED"));
        LS_CHECK(tap("Saved01"));
        ls_scr_wireless.key(LS_TK_CHAR, 'f');
        LS_EQ_INT(operation, LS_WIRELESS_NONE);
        for (const char *p = "Yes"; *p; ++p) ls_keyboard_key(LS_TK_CHAR, *p);
        ls_keyboard_key(LS_TK_ENTER, 0);
        LS_EQ_INT(operation, LS_WIRELESS_FORGET);
        LS_CHECK(!strcmp(joined, "Saved01"));
        for (int i = 0; i < 6; ++i) ls_scr_wireless.key(LS_TK_CHAR, 'k');
        draw();
        LS_CHECK(on_screen("Saved07"));
        ls_scr_wireless.key(LS_TK_CHAR, 'f');
        for (const char *p = "Yes"; *p; ++p) ls_keyboard_key(LS_TK_CHAR, *p);
        ls_keyboard_key(LS_TK_ENTER, 0);
        LS_CHECK(!strcmp(joined, "Saved07"));
        /* A vanished/reordered snapshot must not make selection forget a
         * different SSID at the same numerical index. */
        model.saved_count = 1;
        strcpy(model.saved[0], "Saved00");
        draw();
        operation = LS_WIRELESS_NONE;
        ls_scr_wireless.key(LS_TK_CHAR, 'f');
        LS_EQ_INT(operation, LS_WIRELESS_NONE); /* no implicit current target */
    }
}

const ls_tui_screen_t ls_scr_drones = {.name = "DRONES"};
int ls_tui_screen_index_of(const ls_tui_screen_t *screen) { return screen==&ls_scr_drones ? 7 : -1; }
static int shown_screen = -1;
void ls_tui_screen_show(int index) { shown_screen = index; }
LS_CASE(drones_navigation_uses_router_for_touch_and_keyboard) {
    for (int landscape = 0; landscape < 2; ++landscape) {
        setup(landscape); shown_screen = -1;
        LS_CHECK(!on_screen("DRONES"));                /* not a tab: it is an app */
        ls_scr_wireless.key(LS_TK_CHAR, 'o'); LS_CHECK(opt_row("DRONE WATCH"));
        LS_EQ_INT(shown_screen, ls_tui_screen_index_of(&ls_scr_drones));
        shown_screen = -1; ls_scr_wireless.key(LS_TK_CHAR, 'g');
        LS_EQ_INT(shown_screen, ls_tui_screen_index_of(&ls_scr_drones));
    }
}

LS_CASE(saved_next_touch_connect_and_forget_cancel_keep_named_target) {
    for (int landscape = 0; landscape < 2; ++landscape) {
        setup(landscape); if (!landscape) height=52; model.saved_count=8;
        for(int i=0;i<8;++i) snprintf(model.saved[i],33,"Saved%02d",i);
        shown_screen=-1; draw();
        LS_CHECK(tap("NEXT saved")); draw();
        LS_EQ_INT(shown_screen,-1);
        ls_scr_wireless.key(LS_TK_CHAR,'r');
        LS_EQ_INT(operation,LS_WIRELESS_SAVED);
        LS_CHECK(joined[0] && strcmp(joined,"Saved00"));
        char selected[33]; strcpy(selected,joined);
        operation=LS_WIRELESS_NONE;
        ls_scr_wireless.key(LS_TK_CHAR,'f');
        LS_CHECK(ls_keyboard_active());
        ls_keyboard_key(LS_TK_ESC,0);
        LS_EQ_INT(operation,LS_WIRELESS_NONE);
        ls_scr_wireless.key(LS_TK_CHAR,'f');
        for(const char *p="Yes";*p;++p) ls_keyboard_key(LS_TK_CHAR,*p);
        ls_keyboard_key(LS_TK_ENTER,0);
        LS_CHECK(!strcmp(joined,selected));
    }
}
LS_CASE(bluetooth_options_queue_named_pin_filter_and_telemetry) {
    setup(false); ls_scr_wireless.key(LS_TK_CHAR,'b'); draw();
    LS_CHECK(tap("OPTIONS")); LS_CHECK(opened_options);
    LS_EQ_INT(opened_options->n,4);
    LS_CHECK(opened_options->opt[0].why_not(&opened_options->opt[0]) != NULL);
    model.bt_ready=true; strcpy(model.address,"aa:bb:cc:dd:ee:ff");
    LS_CHECK(opened_options->opt[0].why_not(&opened_options->opt[0]) == NULL);
    opened_options->opt[0].act(&opened_options->opt[0]);
    LS_EQ_INT(operation,LS_WIRELESS_BT_PIN); LS_CHECK(!strcmp(joined,model.address));
    opened_options->opt[2].set_text(&opened_options->opt[2],"my head");
    LS_EQ_INT(operation,LS_WIRELESS_BT_FILTER); LS_CHECK(!strcmp(joined,"my head"));
    opened_options->opt[3].set_num(&opened_options->opt[3],0);
    LS_EQ_INT(operation,LS_WIRELESS_BT_TELEMETRY); LS_CHECK(!strcmp(joined,"0"));
}
LS_CASE(long_status_wraps_inside_border_and_notes_stay_on_their_tab) {
    for(int landscape=0;landscape<2;++landscape) {
        setup(landscape); model.ap_count=0;
        strcpy(model.wifi_status,"Failed \"12345678901234567890123456789012\": authentication mode rejected; JOIN to retry");
        model.operation=LS_WIRELESS_JOIN; model.busy=true;
        draw(); LS_CHECK(!on_screen("Scanning..."));
        LS_CHECK(on_screen("JOIN to retry"));
        for(int y=(landscape?3:4)+1;y<(landscape?3:4)+(landscape?5:6)-1;++y)
            LS_CHECK(back[y*width+width-1].ch!='y');
        model.busy=false; strcpy(model.message,"Saved network removed");
        ls_scr_wireless.key(LS_TK_CHAR,'b'); draw();
        LS_CHECK(!on_screen("Saved network removed"));
        strcpy(model.peer,"Old head"); model.bt_state=0; draw();
        LS_CHECK(!on_screen("Old head"));
    }
}
LS_CASE(password_title_keeps_ssid_through_validation_retry) {
    setup(false); LS_CHECK(tap("Network00")); draw();
    ls_keyboard_draw(&surface,tui_rect_make(0,0,width,height));
    LS_CHECK(on_screen("Network00 / PASSWORD"));
    ls_keyboard_key(LS_TK_ENTER,0); draw();
    ls_keyboard_draw(&surface,tui_rect_make(0,0,width,height));
    LS_CHECK(on_screen("Network00 / USE 8-63 CHARACTERS"));
    ls_keyboard_key(LS_TK_ESC,0);
}

LS_CASE(survey_is_in_wifi_survey_and_v_opens_it)
{
    for (int landscape = 0; landscape < 2; landscape++) {
        setup(landscape);
        /* Three tabs: WI-FI, BLUETOOTH, REMOTE. The survey recorder moved. */
        LS_CHECK(on_screen("REMOTE"));
        LS_CHECK(!on_screen("SURVEY"));
        /* Not on the card: V says where the survey is and stays on WI-FI. */
        card_app_count = 1; opened_app = -1;
        LS_CHECK(ls_scr_wireless.key(LS_TK_CHAR, 'v')); draw();
        LS_EQ_INT(opened_app, -1);
        LS_CHECK(on_screen("Survey is in WIFI SURVEY"));
        /* Installed: V opens it. */
        card_app_count = 2;
        LS_CHECK(ls_scr_wireless.key(LS_TK_CHAR, 'V'));
        LS_EQ_INT(opened_app, 1);
        LS_EQ_INT(operation, LS_WIRELESS_NONE);
        /* TAB walks the three tabs and comes back to WI-FI. */
        ls_scr_wireless.key(LS_TK_CHAR, 'w');
        for (int i = 0; i < 3; i++) ls_scr_wireless.key(LS_TK_TAB, 0);
        draw(); LS_CHECK(on_screen("JOIN"));
    }
}
