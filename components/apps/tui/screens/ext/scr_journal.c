#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_field.h"
#include "../../ls_motion.h"
#include "../../ls_keyboard.h"
#include "../../ls_picker.h"
#include "../../ls_app.h"
#include "../../ls_map.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

EXT_RAM_BSS_ATTR static int button_focus=-1, button_slot;
static ls_field_state_t s;
EXT_RAM_BSS_ATTR static ls_journal_entry_t s_entry;
static char s_title[48], s_feedback[80];
static uint32_t s_edit_id;
static int s_selected, s_scroll;
static bool s_detail, s_sensors;
static tui_rect s_list;
static bool s_touch_nav;
static ls_fresh_t newest;
static int s_scroll_max;

static void feedback(bool ok) { snprintf(s_feedback, sizeof(s_feedback), "%s", ok ? "Queued; SD result appears below" : "Request not accepted; try again after pending save"); }
static void save_text(const char *text) { feedback(ls_field_note(s_edit_id, s_title, text)); }
static void title_done(const char *title)
{
    if (!title[0]) return;
    snprintf(s_title, sizeof(s_title), "%s", title);
    ls_keyboard_open("NOTE", "", LS_JOURNAL_TEXT - 1, save_text);
}
static void source_done(int i) { feedback(ls_field_source((ls_field_source_t)i)); }
static void pick_source(void)
{
    ls_picker_open("ATTACH RADIO", source_done);
    for (int i = 0; i < LS_FIELD_SOURCES; i++)
        ls_picker_add(ls_field_source_name((ls_field_source_t)i), i == LS_FIELD_CC1101 ? "MIX-RF monitor" : i == LS_FIELD_NRF24 ? "2.4 GHz survey" : i == LS_FIELD_NFC ? "External field watch" : i == LS_FIELD_NONE ? "GPS + motion" : "Current tuning");
}
static void action(int i)
{
    s_feedback[0] = 0;
    if (i == 0) { s_edit_id = 0; ls_keyboard_open("NOTE TITLE", "", 47, title_done); }
    else if (i == 1) pick_source();
    else if (i == 2) feedback(ls_field_record(!s.recording));
    else if (i == 3) { s_sensors = !s_sensors; }
    else if (i == 4 && ls_field_entry(s_selected, &s_entry)) {
        s_edit_id = s_entry.id; snprintf(s_title, sizeof(s_title), "%s", s_entry.title);
        ls_keyboard_open("EDIT NOTE", s_entry.text, LS_JOURNAL_TEXT - 1, save_text);
    } else if (i == 5 && ls_field_entry(s_selected, &s_entry) && s_entry.sample.gps_valid) {
        ls_map_center(s_entry.sample.lat, s_entry.sample.lon);
        for (int k = 0; k < ls_app_count(); k++) { const ls_app_t *app = ls_app_at(k); if (app->id && !strcmp(app->id, "map")) { ls_app_open(k); break; } }
    }
}
static void sensors(tui_surface *sf, tui_rect a, const ls_field_sample_t *p)
{
    char line[100];
    snprintf(line, sizeof(line), "%s | %s", ls_field_source_name(p->source), p->radio_valid ? "data attached" : "no radio data");
    ls_kv(sf, a, 1, "RADIO", line, LS_ATTR_DIM);
    if(p->radio_valid && p->frequency) {
        snprintf(line,sizeof(line),"%.4f MHz",p->frequency/1e6);
        ls_kv(sf,a,2,"FREQ",line,LS_ATTR_DIM);
    }
    if (p->gps_valid) snprintf(line, sizeof(line), "%.6f %.6f", p->lat, p->lon);
    else snprintf(line, sizeof(line), "No fresh fix");
    ls_kv(sf, a, 3, "GPS", line, LS_ATTR_DIM);
    if(p->utc[0]) snprintf(line,sizeof(line),"%s",p->utc);
    else snprintf(line,sizeof(line),"uptime %.1f s",p->time_us/1e6);
    ls_kv(sf, a, 4, "TIME", line, LS_ATTR_DIM);
    snprintf(line, sizeof(line), "%s", p->imu_valid ? "9 axis sample attached" : "unavailable");
    ls_kv(sf, a, 6, "MOTION", line, LS_ATTR_DIM);
    if (p->imu_valid) {
        snprintf(line, sizeof(line), "%+.2f %+.2f %+.2f g", p->imu.ax, p->imu.ay, p->imu.az); ls_kv(sf, a, 7, "ACCEL", line, LS_ATTR_DIM);
        snprintf(line, sizeof(line), "%+.1f %+.1f %+.1f d/s", p->imu.gx, p->imu.gy, p->imu.gz); ls_kv(sf, a, 8, "GYRO", line, LS_ATTR_DIM);
        snprintf(line, sizeof(line), "%+.1f %+.1f %+.1f uT", p->imu.mx, p->imu.my, p->imu.mz); ls_kv(sf, a, 9, "MAG", line, LS_ATTR_DIM);
        if (p->imu.mag_valid && isfinite(p->heading)) snprintf(line, sizeof(line), "%.0f magnetic", p->heading); else snprintf(line, sizeof(line), "not available");
        ls_kv(sf, a, 10, "HEADING", line, LS_ATTR_DIM);
        float length = sqrtf(p->imu.ax*p->imu.ax + p->imu.ay*p->imu.ay + p->imu.az*p->imu.az);
        if (a.h > 15) ls_bar(sf, a, 13, 2, a.w - 4, length / 2);
    }
    if (p->signal_valid) {
        if (p->frequency) snprintf(line, sizeof(line), "%.4f MHz  %.1f dBm", p->frequency / 1e6, p->rssi);
        else snprintf(line, sizeof(line), "%.1f dBm; no fixed channel", p->rssi);
        ls_kv(sf, a, a.h - 3, "SIGNAL", line, LS_ATTR_DIM);
    }
}
static void text_rows(tui_surface *sf, tui_rect a, const char *text)
{
    int row = 0, col = 0, width = a.w;
    if (width < 1) return;
    int lines=1,used=0;
    for(const char *p=text;*p;p++){if(*p=='\n'||used>=width){lines++;used=0;if(*p=='\n')continue;}used++;}
    s_scroll_max=lines>a.h?lines-a.h:0;
    if(s_scroll>s_scroll_max)s_scroll=s_scroll_max;
    for (const char *p = text; *p; p++) {
        if (*p == '\n' || col >= width) { row++; col = 0; if (*p == '\n') continue; }
        if (row >= s_scroll && row < s_scroll + a.h)
            tui_put_char(sf, a, a.x + col, a.y + row - s_scroll, *p, LS_ATTR_DIM);
        col++;
    }
}
static void draw(tui_surface *sf, tui_rect a)
{
    if (a.h < 14 || a.w < 24) {
        ls_panel_box(sf, a, "JOURNAL", TUI_CYAN);
        tui_put_str(sf, a, a.x + 1, a.y + 1, "Enlarge the pane", LS_ATTR_DIM);
        return;
    }
    ls_field_snapshot(&s);
    s_touch_nav=a.w<90 && a.h>35;
    if(s_touch_nav){
        ls_btn_t nav[]={{"UP",s_detail?"SCROLL":"ENTRY",0,false,s_sensors || (s_detail?s_scroll==0:s_selected==0)},{"DOWN",s_detail?"SCROLL":"ENTRY",0,false,s_sensors || (s_detail?s_scroll>=s_scroll_max:s_selected+1>=s.journal_count)},{"BACK","LIST",0,false,!s_detail && !s_sensors}};
        ls_btn_bar_raised_slot(sf,tui_rect_make(a.x,a.y+a.h-5,a.w,5),nav,3,-1,LS_BTN_SLOT_WATERFALL);
        a.h-=5;
    }
    if (s_selected >= s.journal_count) s_selected = s.journal_count ? s.journal_count - 1 : 0;
    ls_btn_t buttons[] = {{"NEW", "NOTE", 'n', false, false}, {"RADIO", ls_field_source_name(s.sample.source), 'r', false, false},
        {"RECORD", s.recording ? "ON" : "OFF", 'c', s.recording, false}, {"SENSORS", s_detail ? "ENTRY" : "LIVE", 'v', s_sensors, false},
        {"EDIT", NULL, 'e', false, !s.journal_count}, {"MAP", "ENTRY", 'g', false, !s.journal_count}};
    const int bar_h = ls_btn_raised_height(a, 6);
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y, a.w, bar_h), buttons, 6, button_focus);
    tui_rect panel = tui_rect_make(a.x, a.y + bar_h, a.w, a.h - bar_h - 2);
    ls_panel_box(sf, panel, s_sensors ? (s_detail ? "ENTRY ATTACHMENTS" : "LIVE ATTACHMENTS") : "JOURNAL", TUI_CYAN);
    ls_motion_busy(sf,panel,s.recording);
    if (s_sensors) {
        if(!s_detail) sensors(sf,panel,&s.sample);
        else if(ls_field_entry(s_selected,&s_entry)) sensors(sf,panel,&s_entry.sample);
    }
    else if (s_detail && ls_field_entry(s_selected, &s_entry)) {
        tui_put_str(sf, panel, panel.x + 2, panel.y + 1, s_entry.title, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        text_rows(sf, tui_rect_make(panel.x + 2, panel.y + 3, panel.w - 4, panel.h - 5), s_entry.text);
    } else {
        s_list = tui_rect_make(panel.x + 2, panel.y + 2, panel.w - 4, panel.h - 3);
        if (panel.h > 38) s_list.h = panel.h - 23;
        int rows = s_list.h / 3; if (rows < 1) rows = 1;
        int first = s_selected / rows * rows;
        if (!s.journal_count) {
            tui_put_str(sf, panel, panel.x + 2, panel.y + 3, "A notebook for the field.", LS_ATTR_DIM);
            tui_put_str(sf, panel, panel.x + 2, panel.y + 5, "Write a note, mark a place,", LS_ATTR_DIM);
            tui_put_str(sf, panel, panel.x + 2, panel.y + 6, "or record a walk with sensors.", LS_ATTR_DIM);
            tui_put_str(sf, panel, panel.x + 2, panel.y + 8, "Radio attachments are optional.", LS_ATTR_DIM);
        }
        for (int i = 0; i < rows && ls_field_entry(first + i, &s_entry); i++) {
            int y = s_list.y + i * 3;
            bool selected = first + i == s_selected;
            if (selected) ls_fill_dither(sf, tui_rect_make(s_list.x, y, s_list.w, 2), LS_DITHER_LIGHT, TUI_CYAN);
            uint8_t fresh=first+i==0?ls_fresh(&newest,s_entry.id*2u+s_entry.saved,650):0;
            tui_put_str(sf, panel, s_list.x + 1, y, s_entry.title, ls_fresh_attr(fresh,TUI_GREEN|TUI_BRIGHT,TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
            char sub[90]; snprintf(sub, sizeof(sub), "%s  %s%s%s", s_entry.saved ? "SD" : "RAM", ls_field_source_name(s_entry.sample.source),
                s_entry.sample.gps_valid ? " + GPS" : "", s_entry.sample.imu_valid ? " + IMU" : "");
            tui_put_str(sf, panel, s_list.x + 1, y + 1, sub, LS_ATTR_DIM);
        }
        if (panel.h > 38) {
            tui_rect live = tui_rect_make(panel.x + 1, panel.y + panel.h - 20, panel.w - 2, 19);
            ls_panel_box(sf, live, "LIVE ATTACHMENTS", TUI_CYAN);
            sensors(sf, live, &s.sample);
        }
    }
    char status[100]; snprintf(status, sizeof(status), "%c %s", ls_motion_pip(s.recording), s.storage);
    ls_safe_line(sf, a, a.y + a.h - 2, status, LS_ATTR_DIM);
    ls_safe_line(sf, a, a.y + a.h - 1, s_feedback[0] ? s_feedback : "ENTER read  ARROWS browse  BS back", LS_ATTR_DIM);
}
static void enter(void) { button_focus=-1;button_slot=0; ls_field_start(); ls_field_watch(true); s_detail = s_sensors = false; s_scroll = 0; }
static void leave(void) { ls_field_watch(false); }
static bool key(ls_tk_t k, char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if(k==LS_TK_LEFT || k==LS_TK_RIGHT || k==LS_TK_TAB)return ls_btn_navigate(k,&button_slot,&button_focus,false);
    if(k==LS_TK_ENTER && button_focus>=0){if(ls_btn_enabled(0,button_focus))action(button_focus);return true;}
    if(k==LS_TK_UP || k==LS_TK_DOWN || k==LS_TK_BACKSPACE)button_focus=-1;
    if (k == LS_TK_BACKSPACE) { s_detail = s_sensors = false; return true; }
    if (k == LS_TK_UP) { if (s_detail) { if (s_scroll) s_scroll--; } else if (s_selected) s_selected--; return true; }
    if (k == LS_TK_DOWN) { if (s_detail) { if (s_scroll < s_scroll_max) s_scroll++; } else if (s_selected + 1 < s.journal_count) s_selected++; return true; }
    if (k == LS_TK_ENTER) { s_detail = true; s_sensors = false; s_scroll = 0; return true; }
    if (k != LS_TK_CHAR) return false;
    const char *p = strchr("nrcveg", ch); if (!p) return false; action((int)(p - "nrcveg")); return true;
}
static bool touch(int col, int row)
{
    if(s_touch_nav){int i=ls_btn_hit_slot(col,row,LS_BTN_SLOT_WATERFALL);if(i>=0)return key(i==0?LS_TK_UP:i==1?LS_TK_DOWN:LS_TK_BACKSPACE,0);}
    int button = ls_btn_hit(col, row); if (button >= 0) { action(button); return true; }
    if (s_detail) { if(s_touch_nav)return true; if(s_sensors)s_sensors=false;else s_detail=false; return true; }
    if (s_sensors || col < s_list.x || col >= s_list.x + s_list.w || row < s_list.y || row >= s_list.y + s_list.h) return true;
    int rows = s_list.h / 3; if (rows < 1) rows = 1;
    int index = s_selected / rows * rows + (row - s_list.y) / 3;
    if (index < s.journal_count) { if (index == s_selected) { s_detail = true; s_scroll = 0; } s_selected = index; }
    return true;
}
const ls_tui_screen_t ls_scr_journal = {.name="JOURNAL", .hint="N new  R radio  C record  V sensors", .enter=enter, .leave=leave, .draw=draw, .key=key, .touch=touch};
