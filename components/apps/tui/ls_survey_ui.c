#include "ls_survey_ui.h"
#include "ls_survey.h"
#include "ls_tui_ui.h"
#include "ls_options.h"
#include "ls_motion.h"
#include "ls_radio_panel.h"
#include "ls_glyph.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include "ls_textpack.h"

#define WHITE TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)
#define CYAN TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
static ls_survey_options_t s_options = {true, true, true, 10, 10};
static EXT_RAM_BSS_ATTR ls_survey_view_t s_view;
static ls_fresh_t s_reception;
static EXT_RAM_BSS_ATTR ls_survey_entry_t s_detail;
static unsigned s_pick;
static int s_sort, s_rows = 1, s_top = 3, s_pitch = 3;
static bool s_details;
static tui_rect s_list;

static const char *why(const ls_opt_t *o)
{
    (void)o;
    ls_survey_view(&s_view);
    return s_view.running || s_view.busy ? "Stop the session to change options" : NULL;
}
static int get(const ls_opt_t *o)
{
    return o->arg == 0 ? s_options.wifi : o->arg == 1 ? s_options.ble : s_options.only_fix;
}
static void set(const ls_opt_t *o, int v)
{
    if (o->arg == 0) s_options.wifi = v; else if (o->arg == 1) s_options.ble = v; else s_options.only_fix = v;
}
static double num(const ls_opt_t *o) { return o->arg == 3 ? s_options.interval_s : s_options.keep; }
static void set_num(const ls_opt_t *o, double v)
{
    if (o->arg == 3) s_options.interval_s = (unsigned)v; else s_options.keep = (unsigned)v;
}
static void options_preview(tui_surface *sf, tui_rect a);

static const ls_opt_t RADIOS[] = {
    {.label="SCAN WI-FI", .kind=LS_OPT_TOGGLE, .arg=0, .get=get, .set=set, .why_not=why},
    {.label="LISTEN BLE", .kind=LS_OPT_TOGGLE, .arg=1, .get=get, .set=set, .why_not=why},
    {.label="SCAN INTERVAL", .kind=LS_OPT_LEVEL, .arg=3, .num=num, .set_num=set_num,
     .lo=3, .hi=120, .step=1, .unit="seconds", .why_not=why},
};
static const ls_opt_t RECORDING[] = {
    {.label="ONLY GPS FIX", .kind=LS_OPT_TOGGLE, .arg=2, .get=get, .set=set, .why_not=why},
    {.label="KEEP SESSIONS", .kind=LS_OPT_LEVEL, .arg=4, .num=num, .set_num=set_num,
     .lo=1, .hi=50, .step=1, .unit="sessions", .why_not=why},
};
static const ls_opt_ctx_t CTX_RADIOS = {.name="RADIOS", .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(RADIOS)};
static const ls_opt_ctx_t CTX_RECORDING = {.name="RECORDING", .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(RECORDING)};
static const ls_opt_t OPTIONS[] = {
    {.label="RADIOS", .kind=LS_OPT_MENU, .sub=&CTX_RADIOS},
    {.label="RECORDING", .kind=LS_OPT_MENU, .sub=&CTX_RECORDING},
};
static const ls_opt_ctx_t CTX = {.name="SURVEY", .back_to_screen=true, .preview=options_preview, .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(OPTIONS)};

static void text(tui_surface *sf, tui_rect a, int row, const char *s, uint8_t ink)
{
    ls_safe_line(sf, a, a.y + row, s, ink);
}

/* Inside a panel box: row 0 and row h-1 are its frame. */
static bool fits(tui_rect a, int row) { return row >= 1 && row <= a.h - 2; }
static void inner(tui_surface *sf, tui_rect a, int row, const char *s, uint8_t ink)
{
    if (fits(a, row)) text(sf, a, row, s, ink);
}

static void display_name(char out[33], const char *name)
{
    snprintf(out, 33, "%s", name);
    for (unsigned i = 0; out[i]; i++)
        if ((unsigned char)out[i] < 32 || (unsigned char)out[i] > 126) out[i] = '?';
}

/* Small fixed emblem: antenna above a road, shared block glyphs, no bitmap. */
static void emblem(tui_surface *sf, tui_rect a, bool hot)
{
    for (int y = 0; y < 5; y++) for (int x = 0; x < 11; x++) {
        bool mast = x == 5 && y < 4;
        bool wave = y < 3 && (x == 3-y || x == 7+y);
        bool road = y >= 3 && (x == 7-y || x == 3+y);
        if (mast || wave || road)
            tui_put_char(sf, a, a.x+x, a.y+y, LS_TUI_BLOCK_FULL,
                TUI_ATTR(mast ? TUI_YELLOW | TUI_BRIGHT : road ? TUI_MAGENTA :
                         hot ? TUI_GREEN | TUI_BRIGHT : TUI_CYAN, TUI_BLACK));
    }
}

static void heading(tui_surface *sf, tui_rect a)
{
    const bool wide = ls_tui_is_wide();
    char value[24], line[100];
    bool hot = ls_fresh(&s_reception, s_view.activity_revision, 600) >= 128;
    ls_radio_bezel(sf, a, s_view.busy ? "SURVEY / SAVING" : s_view.running ?
        "SURVEY / LIVE" : s_view.path[0] ? "SURVEY / SAVED" : "SURVEY / READY");
    emblem(sf, tui_rect_make(a.x+2,a.y+1,11,5), hot);
    snprintf(value,sizeof(value),"%u",s_view.wifi+s_view.ble);
    int n = (int)strlen(value);
    if (n <= 7) for (int i=0;i<n;i++)
        ls_glyph_draw(sf,tui_rect_make(a.x+15+i*4,a.y+1,3,5),value[i],WHITE);
    else tui_put_str(sf,a,a.x+15,a.y+2,value,WHITE);
    tui_put_str(sf,a,wide?a.x+49:a.x+15,a.y+(wide?4:6),"OBSERVATIONS / session",LS_ATTR_DIM);
    tui_rect rail = wide ? tui_rect_make(a.x+49,a.y+1,a.w-51,5) :
                          tui_rect_make(a.x+1,a.y+7,a.w-2,2);
    int x=rail.x;
    x=ls_radio_chip(sf,rail,x,rail.y,s_view.running?"REC":"IDLE",s_view.running,TUI_RED);
    x=ls_radio_chip(sf,rail,x,rail.y,"RX",hot,TUI_CYAN);
    x=ls_radio_chip(sf,rail,x,rail.y,s_view.gps_fresh?"GPS FIX":"GPS WAIT",true,s_view.gps_fresh?TUI_GREEN:TUI_YELLOW);
    ls_radio_chip(sf,rail,x,rail.y,s_view.utc_valid?"UTC OK":"UTC WAIT",true,s_view.utc_valid?TUI_GREEN:TUI_YELLOW);
    LS_SNPRINTF(t_ls_survey_ui_u_min_02u_02u, line,sizeof(line),"+%u/min  %02u:%02u:%02u  %u rows",s_view.new_minute,
        s_view.elapsed_s/3600,s_view.elapsed_s/60%60,s_view.elapsed_s%60,s_view.rows);
    text(sf,rail,wide?2:1,line,WHITE);
    if(wide) text(sf,rail,4,s_view.status[0]?s_view.status:"S START / O OPTIONS",LS_ATTR_DIM);
}

static void options_preview(tui_surface *sf, tui_rect a)
{
    char line[64];
    ls_panel_box(sf,a,LS_TEXT(t_ls_survey_ui_survey_recording_plan, "SURVEY / RECORDING PLAN"),TUI_CYAN);
    emblem(sf,tui_rect_make(a.x+2,a.y+2,11,5),false);
    tui_put_str(sf,a,a.x+15,a.y+2,"PASSIVE",WHITE);
    tui_put_str(sf,a,a.x+15,a.y+4,"Wi-Fi + BLE",CYAN);
    text(sf,a,8,s_options.only_fix?"GPS fix required for rows":"Rows allowed without GPS",TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK));
    snprintf(line,sizeof(line),"Wi-Fi %s / BLE %s",s_options.wifi?"ON":"OFF",s_options.ble?"ON":"OFF");
    text(sf,a,10,line,WHITE);
    LS_SNPRINTF(t_ls_survey_ui_scan_every_us_keep, line,sizeof(line),"Scan every %us / keep %u sessions",s_options.interval_s,s_options.keep);
    text(sf,a,12,line,TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK));
    text(sf,a,14,LS_TEXT(t_ls_survey_ui_tap_to_step_value, "Tap < > to step / value to type"),LS_ATTR_DIM);
    text(sf,a,16,LS_TEXT(t_ls_survey_ui_back_esc_returns_to, "BACK / Esc returns to survey"),LS_ATTR_DIM);
    if(a.h>20) {
        text(sf,a,18,LS_TEXT(t_ls_survey_ui_start_records_observations_to, "START records observations to CSV"),CYAN);
        text(sf,a,20,LS_TEXT(t_ls_survey_ui_stop_flushes_the_session, "STOP flushes the session to SD"),TUI_ATTR(TUI_YELLOW,TUI_BLACK));
    }
}

static void mix_row(tui_surface *sf,tui_rect a,int y,const char *label,
                    unsigned value,unsigned total,uint8_t hue)
{
    char line[40];
    if(!fits(a,y)) return;
    snprintf(line,sizeof(line),"%-5s %5u",label,value);
    text(sf,a,y,line,TUI_ATTR(hue|TUI_BRIGHT,TUI_BLACK));
    int width=a.w-17;
    ls_meter_ascii(sf,a,a.x+15,a.y+y,width,total?(float)value/total:0,hue);
}

static void stats(tui_surface *sf, tui_rect a)
{
    char line[100];
    bool tall=a.h>=20;
    ls_panel_box(sf,a,"RADIO MIX / 0..100%",TUI_CYAN);
    LS_SNPRINTF(t_ls_survey_ui_wi_fi_u_u, line,sizeof(line),"Wi-Fi %u +%u/min | BLE %u +%u/min",s_view.wifi,s_view.wifi_minute,s_view.ble,s_view.ble_minute);
    text(sf,a,1,line,WHITE);
    int y=2;
    if(tall) {
        text(sf,a,y++,LS_TEXT(t_ls_survey_ui_new_minute_wi_fi, "NEW / MINUTE     Wi-Fi + BLE"),LS_ATTR_DIM);
        mix_row(sf,a,y++,"Wi-Fi",s_view.wifi_minute,s_view.new_minute,TUI_CYAN);
        mix_row(sf,a,y++,"BLE",s_view.ble_minute,s_view.new_minute,TUI_MAGENTA);
        text(sf,a,y++,LS_TEXT(t_ls_survey_ui_authentication_wi_fi_share, "AUTHENTICATION / Wi-Fi share"),LS_ATTR_DIM);
    }
    mix_row(sf,a,y++,"OPEN",s_view.security[0],s_view.wifi,TUI_RED);
    mix_row(sf,a,y++,"WEP",s_view.security[1],s_view.wifi,TUI_YELLOW);
    mix_row(sf,a,y++,"WPA",s_view.security[2],s_view.wifi,TUI_MAGENTA);
    mix_row(sf,a,y++,"WPA2",s_view.security[3],s_view.wifi,TUI_CYAN);
    mix_row(sf,a,y++,"WPA3",s_view.security[4],s_view.wifi,TUI_GREEN);
    if(tall) text(sf,a,y++,LS_TEXT(t_ls_survey_ui_top_channels_relative_to, "TOP CHANNELS / relative to busiest"),LS_ATTR_DIM);
    for(int i=0;i<3;i++) {
        char label[12];
        if(s_view.top_count[i]) snprintf(label,sizeof(label),"CH %u",s_view.top_channel[i]);
        else snprintf(label,sizeof(label),"CH --");
        mix_row(sf,a,y++,label,s_view.top_count[i],s_view.top_count[0],TUI_GREEN);
    }
    if(tall) {
        LS_SNPRINTF(t_ls_survey_ui_sd_s_llu_mib, line,sizeof(line),"SD %s%llu MiB free / %u rows",s_view.sd_known?"":"? ",
            (unsigned long long)(s_view.free_bytes/(1024*1024)),s_view.rows);
        inner(sf,a,y+1,line,WHITE);
        LS_SNPRINTF(t_ls_survey_ui_cache_u_evicted_u, line,sizeof(line),"Cache %u / evicted %u",s_view.cached,s_view.dropped);
        inner(sf,a,y+2,line,LS_ATTR_DIM);
        inner(sf,a,y+3,LS_TEXT(t_ls_survey_ui_totals_recount_evicted_addresses, "Totals recount evicted addresses"),LS_ATTR_DIM);
    }
}

static void action(int i)
{
    ls_survey_view(&s_view);
    if (i == 0 && !s_view.busy) { if (ls_survey_run(!s_view.running, &s_options)) { s_details = false; s_pick = 0; } }
    if (i == 1) { s_sort = (s_sort + 1) % 3; s_pick = 0; s_details = false; }
    if (i == 2) ls_opt_open(&CTX);
    if (i == 4 || i == 5) {
        unsigned count = s_view.cached;
        if (count) s_pick = (unsigned)(((int)s_pick + (i == 4 ? -s_rows : s_rows) + (int)count * s_rows) % (int)count);
        s_details = false;
    }
    if (i == 3) {
        if (s_details) s_details = false;
        else if (ls_survey_at(s_pick, s_sort, &s_detail)) s_details = true;
    }
}

void ls_survey_draw(tui_surface *sf, tui_rect a)
{
    ls_survey_view(&s_view);
    char line[160], name[33];
    const unsigned count = s_view.cached;
    if (s_pick >= count) s_pick = 0;
    int head_h = ls_tui_is_wide() ? 7 : 10;
    heading(sf, tui_rect_make(a.x,a.y,a.w,head_h));
    int bar_h = ls_tui_is_wide() ? 3 : 6;
    tui_rect body = tui_rect_make(a.x, a.y + head_h, a.w, a.h - head_h - bar_h - 3);
    tui_rect art;
    if(ls_tui_is_wide()) {
        art=tui_rect_make(body.x,body.y,body.w/2,body.h);
        s_list=tui_rect_make(body.x+art.w,body.y,body.w-art.w,body.h);
    } else {
        int h=body.h>=30?21:body.h/2;
        art=tui_rect_make(body.x,body.y,body.w,h);
        s_list=tui_rect_make(body.x,body.y+h,body.w,body.h-h);
    }
    stats(sf, art);
    ls_panel_box(sf, s_list, s_details ? "OBSERVATION / BEST SIGNAL" : "SEEN / SELECT FOR DETAILS", TUI_CYAN);
    if (s_details) {
        display_name(name, s_detail.name);
        text(sf, s_list, 1, name[0] ? name : "(no advertised name / hidden SSID)", WHITE);
        LS_SNPRINTF(t_ls_survey_ui_s_02x_02x_02x, line, sizeof(line), "%s %02x:%02x:%02x:%02x:%02x:%02x", s_detail.kind == LS_SURVEY_WIFI ? "Wi-Fi" : "BLE",
                 s_detail.addr[0], s_detail.addr[1], s_detail.addr[2], s_detail.addr[3], s_detail.addr[4], s_detail.addr[5]);
        text(sf, s_list, 2, line, CYAN);
        if (s_detail.kind == LS_SURVEY_WIFI) LS_SNPRINTF(t_ls_survey_ui_d_dbm_ch_u, line, sizeof(line), "%d dBm  CH %u  Auth %u", s_detail.rssi, s_detail.channel, s_detail.auth);
        else LS_SNPRINTF(t_ls_survey_ui_d_dbm_address_type, line, sizeof(line), "%d dBm  Address type %u", s_detail.rssi, s_detail.addr_type);
        text(sf, s_list, 3, line, WHITE);
        LS_SNPRINTF(t_ls_survey_ui_first_lu_last_lu, line, sizeof(line), "First %lu  Last %lu (%s)", (unsigned long)s_detail.first_s, (unsigned long)s_detail.last_s, s_detail.epoch ? "UTC seconds" : "uptime");
        text(sf, s_list, 4, line, LS_ATTR_DIM);
        if (s_detail.positioned) snprintf(line, sizeof(line), "Best: %.6f, %.6f", s_detail.lat, s_detail.lon);
        else LS_SNPUTS(t_ls_survey_ui_best_signal_has_no, line, sizeof(line), "Best signal has no fresh GPS position");
        text(sf, s_list, 5, line, WHITE);
        LS_SNPRINTF(t_ls_survey_ui_u_sightings_strongest_recorded, line,sizeof(line),"%u sightings / strongest recorded signal",s_detail.samples);
        text(sf,s_list,6,line,LS_ATTR_DIM);
        ls_meter_ascii(sf,s_list,s_list.x+2,s_list.y+8,s_list.w-12,
            (s_detail.rssi+100)/80.0f,TUI_GREEN);
        text(sf,s_list,9,LS_TEXT(t_ls_survey_ui_100_60_20_dbm, "-100 / -60 / -20 dBm"),LS_ATTR_DIM);
        if(s_list.h>15) {
            text(sf,s_list,12,LS_TEXT(t_ls_survey_ui_signal_100_to_20, "SIGNAL / -100 to -20 dBm"),CYAN);
            text(sf,s_list,14,LS_TEXT(t_ls_survey_ui_position_belongs_to_the, "Position belongs to the best signal"),WHITE);
            text(sf,s_list,13,s_detail.kind==LS_SURVEY_BLE?"Random BLE addresses rotate":"BSSID identifies this access point",LS_ATTR_DIM);
        }
    } else {
        static const char *SORT[] = {"STRONGEST", "NEWEST", "NAME"};
        /* Every observation has a filled, three-row touch target. */
        const bool tight = s_list.h < 14;
        s_top = tight ? 2 : 3;
        s_pitch = 3;
        s_rows = (s_list.h - s_top - 1) / s_pitch;
        if (s_rows < 1) s_rows = 1;
        unsigned start = s_pick / s_rows * s_rows;
        unsigned end = start + s_rows;
        if (end > count) end = count;
        snprintf(line, sizeof(line), "Sort %s  %u-%u / %u", SORT[s_sort], count ? start + 1 : 0, end, count);
        text(sf, s_list, 1, line, LS_ATTR_DIM);
        if (!count) {
            inner(sf,s_list,s_top,"READY TO EXPLORE",CYAN);
            inner(sf,s_list,s_top+2,LS_TEXT(t_ls_survey_ui_s_start_passive_wi, "S START / passive Wi-Fi + BLE"),WHITE);
            inner(sf,s_list,s_top+4,LS_TEXT(t_ls_survey_ui_gps_stamps_the_strongest, "GPS stamps the strongest sighting"),LS_ATTR_DIM);
            inner(sf,s_list,s_top+5,LS_TEXT(t_ls_survey_ui_o_options_choose_radios, "O OPTIONS / choose radios + logging"),LS_ATTR_DIM);
            if(fits(s_list,s_top+11)) {
                text(sf,s_list,s_top+7,LS_TEXT(t_ls_survey_ui_1_check_gps_fix, "1  Check GPS FIX and UTC lamps"),TUI_ATTR(TUI_GREEN,TUI_BLACK));
                text(sf,s_list,s_top+9,LS_TEXT(t_ls_survey_ui_2_start_new_addresses, "2  START / new addresses count up"),CYAN);
                text(sf,s_list,s_top+11,LS_TEXT(t_ls_survey_ui_3_stop_flush_and, "3  STOP / flush and save session"),TUI_ATTR(TUI_MAGENTA,TUI_BLACK));
            }
        }
        for (int i = 0; i < s_rows; i++) {
            ls_survey_entry_t e;
            if (!ls_survey_at(start + i, s_sort, &e)) break;
            tui_rect row = tui_rect_make(s_list.x + 1, s_list.y + s_top + i * s_pitch, s_list.w - 2, s_pitch);
            ls_fill_dither(sf, row, start+i==s_pick ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, e.kind==LS_SURVEY_WIFI?TUI_CYAN:TUI_MAGENTA);
            display_name(name, e.name);
            snprintf(line, sizeof(line), "%c %s %.32s", start + i == s_pick ? '>' : ' ', e.kind == LS_SURVEY_WIFI ? "W" : "B", name[0] ? name : "(unnamed)");
            text(sf, row, 0, line, WHITE);
            LS_SNPRINTF(t_ls_survey_ui_d_dbm_02x_02x, line, sizeof(line), "%d dBm  %02x:%02x:%02x:%02x:%02x:%02x %s", e.rssi,
                e.addr[0], e.addr[1], e.addr[2], e.addr[3], e.addr[4], e.addr[5], e.positioned ? "GPS" : "--");
            text(sf, row, 1, line, CYAN);
        }
    }
    const char *filename = strrchr(s_view.path, '/');
    snprintf(line, sizeof(line), "File: %s", s_view.path[0] ?
        (filename ? filename + 1 : s_view.path) : "waiting for GPS UTC");
    text(sf, a, a.h - bar_h - 3, line, LS_ATTR_DIM);
    LS_SNPRINTF(t_ls_survey_ui_sd_s_llu_mib_2, line,sizeof(line),"SD %s%llu MiB free | %u rows | %u evicted",s_view.sd_known?"":"? ",
        (unsigned long long)(s_view.free_bytes/(1024*1024)),s_view.rows,s_view.dropped);
    text(sf,a,a.h-bar_h-2,line,WHITE);
    /* Portrait has no rail row for the status, so a refusal or result takes
       the key hint's place; landscape shows it in the heading. */
    if (!ls_tui_is_wide() && s_view.status[0])
        text(sf, a, a.h - bar_h - 1, s_view.status, TUI_ATTR(TUI_YELLOW, TUI_BLACK));
    else
        text(sf, a, a.h - bar_h - 1, LS_TEXT(t_ls_survey_ui_s_run_r_sort, "S run  R sort  O options  D detail  [ ] page"), LS_ATTR_DIM);
    ls_btn_t buttons[] = {
        {s_view.busy ? "SAVING" : s_view.running ? "STOP" : "START", NULL, 's', s_view.running, s_view.busy, false},
        {"SORT", NULL, 'r', false, false, false},
        {"OPTIONS", NULL, 'o', false, false, false},
        {s_details ? "BACK" : "DETAIL", NULL, 'd', s_details, !count, false},
        {"PREV", NULL, '[', false, !count, false},
        {"NEXT", NULL, ']', false, !count, false},
    };
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y + a.h - bar_h, a.w, bar_h), buttons, 6, -1);
}

bool ls_survey_key(ls_tk_t k, char ch)
{
    if (k == LS_TK_ESC && s_details) { s_details = false; return true; }
    if (k == LS_TK_CHAR && ch && strchr("sS", ch) && s_view.busy) return true;
    if (k == LS_TK_ENTER) { action(3); return true; }
    if (k == LS_TK_UP || k == LS_TK_DOWN || ch == '[' || ch == ']') {
        unsigned count = s_view.cached;
        int step = k == LS_TK_UP ? -1 : k == LS_TK_DOWN ? 1 : ch == '[' ? -s_rows : s_rows;
        if (count) s_pick = (unsigned)(((int)s_pick + step + (int)count * s_rows) % (int)count);
        s_details = false;
        return true;
    }
    if (k == LS_TK_CHAR) {
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        const char *p = ch ? strchr("srod", ch) : NULL;
        if (p) { action((int)(p - "srod")); return true; }
    }
    return false;
}

bool ls_survey_touch(int x, int y)
{
    int b = ls_btn_hit(x, y);
    if (b >= 0) { action(b); return true; }
    if (!s_details && x >= s_list.x && x < s_list.x + s_list.w && y >= s_list.y + s_top && y < s_list.y + s_top + s_rows * s_pitch) {
        unsigned rank = s_pick / s_rows * s_rows + (y - s_list.y - s_top) / s_pitch;
        if (ls_survey_at(rank, s_sort, &s_detail)) { s_pick = rank; s_details = true; }
    }
    return true;
}
