#include "ls_test.h"
#include "ls_wireless.h"
#include "ls_tui_screen.h"
#include "ls_keyboard.h"
#include "ls_options.h"
#include "ls_survey.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const ls_tui_screen_t ls_scr_wireless;
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
/* SURVEY's panel frame and chips, without the radio panel and its scanner. */
void ls_radio_bezel(tui_surface *sf, tui_rect r, const char *title)
{ tui_box(sf, r, title, TUI_ATTR(TUI_CYAN, TUI_BLACK)); }
int ls_radio_chip(tui_surface *sf, tui_rect r, int x, int y, const char *t, bool on, uint8_t hue)
{ (void)on; tui_put_str(sf, r, x, y, t, TUI_ATTR(hue, TUI_BLACK)); return x + (int)strlen(t) + 3; }
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
    /* Buttons carry their key only while a keyboard is attached. */
    const bool bottom_button = text[0] == '[' && text[1] != 'B' && text[1] != 'W';
    if (!keyboard && text[0] == '[' && strlen(text) > 4 && text[2] == ']') text += 4;
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
        LS_CHECK(tap("[R] AUTO")); LS_EQ_INT(operation, LS_WIRELESS_SAVED);
        LS_CHECK(tap("[D] LEAVE")); LS_EQ_INT(operation, LS_WIRELESS_LEAVE);
        operation = LS_WIRELESS_NONE;
        ls_scr_wireless.key(LS_TK_CHAR, 'f'); LS_EQ_INT(operation, LS_WIRELESS_NONE);
        LS_CHECK(tap("[B] BLUETOOTH")); draw();
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
    LS_CHECK(tap("[M] MANUAL"));
    const char *ssid = "HiddenNet";
    for (const char *p = ssid; *p; p++) ls_keyboard_key(LS_TK_CHAR, *p);
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_CHECK(ls_keyboard_active());
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_EQ_INT(operation, LS_WIRELESS_JOIN);
    LS_CHECK(!strcmp(joined, ssid)); LS_CHECK(!password[0]);
    operation = LS_WIRELESS_NONE;
    LS_CHECK(tap("[M] MANUAL"));
    ls_keyboard_key(LS_TK_ESC, 0);
    LS_EQ_INT(operation, LS_WIRELESS_NONE);
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
void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch)
{
    if (cols) *cols = width;
    if (rows) *rows = height;
    if (cw) *cw = 10;
    if (ch) *ch = 17;
}

static ls_survey_view_t survey;
static ls_survey_entry_t survey_entries[12];
bool ls_survey_run(bool start, const ls_survey_options_t *options)
{
    survey.running = start;
    survey.options = *options;
    if (!start) {
        strcpy(survey.path, "/sdcard/lakeshark/survey/20261007_120000.csv");
        strcpy(survey.status, "Session saved / random BLE addresses rotate");
    }
    return true;
}
void ls_survey_view(ls_survey_view_t *out) { *out = survey; }
bool ls_survey_at(unsigned rank, int sort, ls_survey_entry_t *out)
{
    (void)sort;
    if (rank >= survey.wifi + survey.ble) return false;
    *out = survey_entries[rank];
    return true;
}

static void survey_dump(const char *page)
{
    const char *folder = getenv("LS_SURVEY_DUMP");
    if (!folder) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/survey-%s-%s.cells", folder, wide ? "landscape" : "portrait", page);
    FILE *f = fopen(path, "wb");
    LS_CHECK(f != NULL); if (!f) return;
    fprintf(f, "%d %d\n", width, height);
    for (int i = 0; i < width * height; i++) { fputc((unsigned char)back[i].ch, f); fputc(back[i].attr, f); }
    fclose(f);
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

LS_CASE(survey_list_uses_the_height_and_start_failures_fit_in_both_postures)
{
    for (int landscape = 0; landscape < 2; landscape++) {
        setup(landscape);
        memset(&survey, 0, sizeof(survey));
        survey.wifi = 6; survey.ble = 6; survey.cached = 12; survey.gps_fresh = true;
        for (int i = 0; i < 12; i++) {
            survey_entries[i] = (ls_survey_entry_t){.kind = LS_SURVEY_WIFI, .rssi = -40 - i};
            snprintf(survey_entries[i].name, 33, "Survey%02d", i);
        }
        /* The reasons ls_survey_runtime.c reports, each whole on one line. */
        static const char *const WHY[] = {
            "Wi-Fi is on: turn STA and AP off first", "BLE link is on: turn it off first",
            "SD card not writable", "GPS would not start (fix-only is on)" };
        LS_CHECK(tap("SURVEY")); draw();
        for (unsigned i = 0; i < sizeof(WHY) / sizeof(WHY[0]); i++) {
            snprintf(survey.status, sizeof(survey.status), "%s", WHY[i]);
            draw(); LS_CHECK_MSG(on_screen(WHY[i]), "cut: %s", WHY[i]);
        }
        survey_dump("list");
        /* Three-row touch targets: five fit in portrait, one in this short landscape. */
        LS_CHECK(on_screen(landscape ? "Survey00" : "Survey04"));
        LS_CHECK(on_screen(landscape ? "1-1 / 12" : "1-5 / 12"));
        snprintf(survey.status, sizeof(survey.status), "%s", WHY[0]);
        draw(); survey_dump("fail");
    }
}

LS_CASE(survey_touch_and_keys_never_request_join_or_head_scan)
{
    for (int landscape = 0; landscape < 2; landscape++) {
        setup(landscape);
        memset(&survey, 0, sizeof(survey));
        survey.wifi = 6; survey.ble = 6; survey.cached = 12; survey.gps_fresh = true;
        strcpy(survey.status, "Passive survey / GPS fresh");
        for (int i = 0; i < 12; i++) {
            survey_entries[i] = (ls_survey_entry_t){.kind=i < 6 ? LS_SURVEY_WIFI : LS_SURVEY_BLE, .rssi=-40-i};
            survey_entries[i].addr[5] = i + 1;
            survey_entries[i].channel = 6; survey_entries[i].auth = 3;
            survey_entries[i].positioned = true; survey_entries[i].lat = 42; survey_entries[i].lon = -71;
            snprintf(survey_entries[i].name, 33, "Survey%02d", i);
        }
        operation = LS_WIRELESS_NONE;
        LS_CHECK(tap("SURVEY")); draw();
        LS_CHECK(tap("START")); draw(); LS_CHECK(survey.running); survey_dump("walk");
        LS_CHECK(tap("[N] NEXT")); draw();
        LS_CHECK(tap("[D] DETAIL")); draw(); survey_dump("detail");
        LS_CHECK(tap("[D] BACK")); draw();
        LS_CHECK(tap("OPTIONS"));
        LS_CHECK(opened_options && !strcmp(opened_options->name, "SURVEY"));
        LS_EQ_INT(opened_options->n, 2);
        LS_EQ_INT(opened_options->radio, LS_RSEL_NONE);
        LS_EQ_INT(opened_options->opt[0].kind, LS_OPT_MENU);
        const ls_opt_ctx_t *radios = opened_options->opt[0].sub;
        LS_EQ_INT(radios->n, 3);
        LS_EQ_INT(radios->opt[2].kind, LS_OPT_LEVEL);
        LS_CHECK(radios->opt[0].why_not(&radios->opt[0]) != NULL);
        LS_CHECK(tap("STOP")); draw(); LS_CHECK(!survey.running); survey_dump("summary");
        LS_CHECK(radios->opt[0].why_not(&radios->opt[0]) == NULL);
        const ls_opt_t *interval = &radios->opt[2];
        interval->set_num(interval, 12); LS_NEAR(interval->num(interval), 12, 0);
        interval->set_num(interval, 10);
        const ls_opt_ctx_t *recording = opened_options->opt[1].sub;
        LS_EQ_INT(recording->n, 2);
        LS_EQ_INT(recording->opt[1].kind, LS_OPT_LEVEL);
        LS_EQ_INT(operation, LS_WIRELESS_NONE);
    }
}

const ls_tui_screen_t ls_scr_drones = {.name = "DRONES"};
int ls_tui_screen_index_of(const ls_tui_screen_t *screen) { return screen==&ls_scr_drones ? 7 : -1; }
static int shown_screen = -1;
void ls_tui_screen_show(int index) { shown_screen = index; }
LS_CASE(drones_navigation_uses_router_for_touch_and_keyboard) {
    for (int landscape = 0; landscape < 2; ++landscape) {
        setup(landscape); shown_screen = -1;
        LS_CHECK(tap("DRONES"));
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
LS_CASE(survey_idle_is_static_until_an_observation_arrives) {
    setup(false); memset(&survey,0,sizeof(survey)); survey.running=true;
    ls_scr_wireless.key(LS_TK_CHAR,'v'); draw();
    static tui_cell copy[128*72]; memcpy(copy,back,sizeof(copy));
    ls_shim_time_set(123000000); draw(); survey_dump("idle");
    LS_CHECK(!memcmp(copy,back,width*height*sizeof(back[0])));
    LS_CHECK(on_screen("SURVEY / LIVE"));
    survey.activity_revision++; draw();
    LS_CHECK(memcmp(copy,back,width*height*sizeof(back[0])));   /* the observation pulses */
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

LS_CASE(survey_stats_show_real_counts_file_and_space_in_both_postures)
{
    for (int landscape=0; landscape<2; landscape++) {
        setup(landscape); memset(&survey,0,sizeof(survey));
        survey.wifi=100; survey.ble=20; survey.wifi_minute=7; survey.ble_minute=3;
        survey.security[0]=2; survey.security[1]=1; survey.security[2]=4;
        survey.security[3]=80; survey.security[4]=13;
        survey.top_channel[0]=6; survey.top_count[0]=30;
        survey.top_channel[1]=1; survey.top_count[1]=20;
        survey.top_channel[2]=11; survey.top_count[2]=10;
        survey.rows=125; survey.free_bytes=512*1024*1024; survey.sd_known=true;
        survey.gps_fresh=survey.utc_valid=true;
        strcpy(survey.path,"/sdcard/lakeshark/survey/20261007_120000.csv");
        ls_scr_wireless.key(LS_TK_CHAR,'v'); draw(); survey_dump("stats");
        LS_CHECK(on_screen("Wi-Fi 100 +7/min | BLE 20 +3/min"));
        LS_CHECK(on_screen("OPEN      2")); LS_CHECK(on_screen("WEP       1"));
        LS_CHECK(on_screen("WPA       4")); LS_CHECK(on_screen("WPA2     80"));
        LS_CHECK(on_screen("WPA3     13"));
        if (!landscape) {   /* the short landscape panel has no room for channels */
            LS_CHECK(on_screen("CH 6     30")); LS_CHECK(on_screen("CH 1     20"));
            LS_CHECK(on_screen("CH 11    10"));
        }
        LS_CHECK(on_screen("SD 512 MiB free | 125 rows"));
        LS_CHECK(on_screen("GPS FIX")); LS_CHECK(on_screen("UTC OK"));
        LS_CHECK(on_screen("File: 20261007_120000.csv"));
    }
}
