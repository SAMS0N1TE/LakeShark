#include "ls_test.h"
#include "ls_wireless.h"
#include "ls_tui_screen.h"
#include "ls_keyboard.h"
#include <stdio.h>
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
    for (int y = 0; y < height; y++) {
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
    for (int landscape = 0; landscape < 2; landscape++) {
        setup(landscape);
        LS_CHECK(active);
        LS_CHECK(tap("[S] SCAN")); LS_EQ_INT(operation, LS_WIRELESS_SCAN);
        LS_CHECK(tap("[R] SAVED")); LS_EQ_INT(operation, LS_WIRELESS_SAVED);
        LS_CHECK(tap("[D] LEAVE")); LS_EQ_INT(operation, LS_WIRELESS_LEAVE);
        LS_CHECK(tap("[F] FORGET")); LS_EQ_INT(operation, LS_WIRELESS_FORGET);
        LS_CHECK(tap("[B] BLUETOOTH")); draw();
        LS_CHECK(tap("[S] SCAN")); LS_EQ_INT(operation, LS_WIRELESS_BT_RESCAN);
        LS_CHECK(tap("[D] STOP")); LS_EQ_INT(operation, LS_WIRELESS_BT_STOP);
        ls_scr_wireless.leave(); LS_CHECK(!active);
    }
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
