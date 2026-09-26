#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_field.h"
#include "../../ls_compass.h"
#include "../../ls_motion.h"
#include "../../ls_keyboard.h"
#include "../../ls_numpad.h"
#include "../../ls_picker.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

EXT_RAM_BSS_ATTR static int button_focus=-1, button_slot;
EXT_RAM_BSS_ATTR static ls_field_state_t s;
static int s_setting;
static char s_feedback[72];
static bool s_hold;
static bool s_guide_visible;
static bool s_full_compass;
static bool s_embedded;
static bool s_show_signal = true;
static int64_t s_full_open_us;
static tui_rect s_expand_hit;
static float s_heading = NAN;
static int64_t s_heading_us;
static ls_fresh_t packet_arrival;
EXT_RAM_BSS_ATTR static float s_trace[LS_FIELD_BINS], s_spectrum[LS_FIELD_BINS], s_bearing[36];
EXT_RAM_BSS_ATTR static uint16_t s_bearing_count[36];
static const char *const modes[] = {"PACKETS", "SPECTRUM", "BEARING",
                                    "GFSK", "POCSAG"};
#define MODE_N ((int)(sizeof(modes) / sizeof(modes[0])))
static const char *const settings[] = {"FREQUENCY", "SPREAD FACTOR", "BANDWIDTH", "CODING RATE", "POWER", "PREAMBLE", "SYNC WORD", "CRC", "INVERT IQ"};
static const uint32_t bandwidths[] = {7810, 10420, 15630, 20830, 31250, 41670, 62500, 125000, 250000, 500000};

static void result(bool ok) { snprintf(s_feedback, sizeof(s_feedback), "%s", ok ? "Request queued" : "Not accepted; check value or wait for worker"); }
static void expand_compass(bool open)
{
    s_full_compass = open; s_full_open_us = esp_timer_get_time();
    button_focus = -1; button_slot = 0;
}
static void set_number(double n)
{
    ls_lora_cfg_t cfg = s.config;
    if (!isfinite(n)) { result(false); return; }
    switch (s_setting) {
    case 0: if (n < 150 || n > 959) { result(false); return; } cfg.freq_hz = (uint32_t)llround(n * 1e6); cfg.cal_min_mhz = (uint16_t)(n / 4) * 4; cfg.cal_max_mhz = cfg.cal_min_mhz + 4; break;
    case 1: if (n < 5 || n > 12 || n != floor(n)) { result(false); return; } cfg.sf = (uint8_t)n; break;
    case 3: if (n < 5 || n > 8 || n != floor(n)) { result(false); return; } cfg.cr = (uint8_t)n; break;
    case 4: if (n < -9 || n > 22 || n != floor(n)) { result(false); return; } cfg.power_dbm = (int8_t)n; break;
    case 5: if (n < 1 || n > 65535 || n != floor(n)) { result(false); return; } cfg.preamble = (uint16_t)n; break;
    case 6: if (n < 0 || n > 255 || n != floor(n)) { result(false); return; } cfg.sync_word = (uint8_t)n; break;
    default: return;
    }
    result(ls_field_configure(&cfg));
}
static void set_bw(int i) { if (i < 0 || i >= 10) return; ls_lora_cfg_t cfg = s.config; cfg.bw_hz = bandwidths[i]; result(ls_field_configure(&cfg)); }

/* Somewhere useful to start, per band. This screen tunes ONE frequency, so
   these are points rather than the ranges ls_wf_source.c sweeps for the
   waterfall - the picker's second column carries the exact figure. All of
   them sit inside the 150-959 MHz the numeric entry already enforces. */
static const struct { const char *name; double mhz; } LABS_BANDS[] = {
    { "mesh US",     910.525 },
    { "mesh EU",     869.525 },
    { "US915 low",   902.300 },
    { "US915 mid",   915.000 },
    { "US915 high",  927.500 },
    { "EU868",       868.100 },
    { "433 ISM",     433.175 },
    { "315 remotes", 315.000 },
    /* Reachable because the part tunes down to 150 MHz, and the obvious place
       to point POCSAG mode. Nothing to do with LoRa. */
    { "VHF pager",   152.600 },
};
#define LABS_BAND_N ((int)(sizeof(LABS_BANDS) / sizeof(LABS_BANDS[0])))

/* The same move the numeric entry makes for frequency, including the 4 MHz
   calibration window - a retune with a stale window is what makes the part
   come back deaf. */
static void set_band(int i)
{
    if (i < 0 || i >= LABS_BAND_N) return;
    const double mhz = LABS_BANDS[i].mhz;
    ls_lora_cfg_t cfg = s.config;
    cfg.freq_hz = (uint32_t)llround(mhz * 1e6);
    cfg.cal_min_mhz = (uint16_t)(mhz / 4) * 4;
    cfg.cal_max_mhz = cfg.cal_min_mhz + 4;
    result(ls_field_configure(&cfg));
}

static void open_band_picker(void)
{
    ls_picker_open("LORA BAND", set_band);
    for (int i = 0; i < LABS_BAND_N; i++) {
        char detail[16];
        snprintf(detail, sizeof(detail), "%.4f", LABS_BANDS[i].mhz);
        ls_picker_add(LABS_BANDS[i].name, detail);
    }
}

/* What REC should read. A count rather than "ON", because the question you
   actually have while walking around is whether anything is landing on the
   card. PACKETS mode counts payloads into packets.csv; the other two modes
   produce no packets at all, so they count sample rows instead rather than
   sitting on a permanent zero. */
static const char *rec_face(void)
{
    static char buf[12];
    if (!s.recording) return "OFF";
    if (s.mode == LS_LAB_PACKETS)
        snprintf(buf, sizeof(buf), "%lu PKT", (unsigned long)s.record_packets);
    else
        snprintf(buf, sizeof(buf), "%lu ROW", (unsigned long)s.record_rows);
    return buf;
}

/* The decoded pages, newest first, the way the FM screen lists them. UP and
   DOWN walk the selection; the selected message gets its own full-width line
   underneath, because eighty characters do not fit on a list row. */
static int s_page_sel;

static void pages_list(tui_surface *sf, tui_rect r)
{
    const int count = s.page_log_count;
    if (!count) {
        tui_put_str(sf, r, r.x + 1, r.y + 1,
                    s.direct ? "Listening for pages"
                             : "Enable DIRECT to listen; Mesh will pause",
                    TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        return;
    }
    if (s_page_sel >= count) s_page_sel = count - 1;
    if (s_page_sel < 0) s_page_sel = 0;

    /* Two rows held back for the selected message and its detail line. */
    int rows = r.h - 3;
    if (rows > count) rows = count;
    if (rows < 1) rows = 1;
    int top = s_page_sel - rows + 1;
    if (top < 0) top = 0;

    char row[96];
    for (int i = 0; i < rows && top + i < count; i++) {
        const ls_field_page_t *p = &s.page_log[top + i];
        const bool on = top + i == s_page_sel;
        snprintf(row, sizeof(row), "%c %lu  %s", on ? '>' : ' ',
                 (unsigned long)p->address, p->text);
        tui_put_str(sf, r, r.x, r.y + i, row,
                    on ? TUI_ATTR(TUI_BLACK, TUI_CYAN) : LS_ATTR_DIM);
    }
    const ls_field_page_t *sel = &s.page_log[s_page_sel];
    snprintf(row, sizeof(row), "%d of %d | addr %lu | f%u | %u bd",
             s_page_sel + 1, count, (unsigned long)sel->address,
             sel->function, sel->baud);
    tui_put_str(sf, r, r.x, r.y + r.h - 2, row, LS_ATTR_DIM);
    tui_put_str(sf, r, r.x, r.y + r.h - 1, sel->text,
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

/* The preset the radio is sitting on, or the bare frequency when it is
   somewhere the list has no name for. */
static const char *band_now(void)
{
    static char buf[16];
    for (int i = 0; i < LABS_BAND_N; i++)
        if ((uint32_t)llround(LABS_BANDS[i].mhz * 1e6) == s.config.freq_hz)
            return LABS_BANDS[i].name;
    snprintf(buf, sizeof(buf), "%.3f", s.config.freq_hz / 1e6);
    return buf;
}
static void edit_setting(int i)
{
    if (i < 0 || i >= 9) return;
    s_setting = i;
    if (i == 2) {
        ls_picker_open("BANDWIDTH", set_bw);
        for (int k = 0; k < 10; k++) { char label[24]; snprintf(label, sizeof(label), "%.2f kHz", bandwidths[k] / 1000.0); ls_picker_add(label, ""); }
    } else if (i == 7 || i == 8) {
        ls_lora_cfg_t cfg = s.config;
        if (i == 7) cfg.crc_on = !cfg.crc_on; else cfg.invert_iq = !cfg.invert_iq;
        result(ls_field_configure(&cfg));
    } else {
        double values[] = {s.config.freq_hz / 1e6, s.config.sf, 0, s.config.cr, s.config.power_dbm, s.config.preamble, s.config.sync_word};
        ls_numpad_open(settings[i], i == 0 ? "MHz" : i == 4 ? "dBm" : i == 6 ? "decimal" : "", values[i], set_number);
    }
}
static void setup(void)
{
    ls_picker_open("LORA SETTINGS", edit_setting);
    char value[32];
    for (int i = 0; i < 9; i++) {
        switch (i) {
        case 0: snprintf(value, sizeof(value), "%.4f MHz", s.config.freq_hz / 1e6); break;
        case 1: snprintf(value, sizeof(value), "SF%u", s.config.sf); break;
        case 2: snprintf(value, sizeof(value), "%.2f kHz", s.config.bw_hz / 1000.0); break;
        case 3: snprintf(value, sizeof(value), "4/%u", s.config.cr); break;
        case 4: snprintf(value, sizeof(value), "%d dBm", s.config.power_dbm); break;
        case 5: snprintf(value, sizeof(value), "%u symbols", s.config.preamble); break;
        case 6: snprintf(value, sizeof(value), "0x%02X", s.config.sync_word); break;
        case 7: snprintf(value, sizeof(value), "%s", s.config.crc_on ? "on" : "off"); break;
        default: snprintf(value, sizeof(value), "%s", s.config.invert_iq ? "on" : "off"); break;
        }
        ls_picker_add(settings[i], value);
    }
}
/* An FSK mode has its own parameters, so SETUP shows a different list. In
   POCSAG only the first two appear: the rest are pinned by fsk_params in
   ls_field.c, and offering a control that the next retune overwrites is the
   lit-control-that-does-nothing fault. Baud is offered because 1200 and 2400
   are both in use. */
static const char *const fsk_settings[] = {"FREQUENCY", "BITRATE", "DEVIATION",
                                           "BANDWIDTH", "SYNC WORD", "PAYLOAD"};
#define FSK_SETTING_N ((int)(sizeof(fsk_settings) / sizeof(fsk_settings[0])))
static int s_fsk_setting;

static void set_fsk_number(double n)
{
    if (!isfinite(n)) { result(false); return; }
    ls_fsk_cfg_t cfg = s.fsk;
    switch (s_fsk_setting) {
    case 0: {
        /* Frequency lives on the LoRa config, so one BAND choice moves every
           mode rather than each carrying its own. */
        if (n < 150 || n > 959) { result(false); return; }
        ls_lora_cfg_t tuned = s.config;
        tuned.freq_hz = (uint32_t)llround(n * 1e6);
        tuned.cal_min_mhz = (uint16_t)(n / 4) * 4;
        tuned.cal_max_mhz = tuned.cal_min_mhz + 4;
        result(ls_field_configure(&tuned));
        return;
    }
    case 1: cfg.bitrate = (uint32_t)llround(n); break;
    case 2: cfg.deviation_hz = (uint32_t)llround(n); break;
    case 3: cfg.bandwidth_hz = (uint32_t)llround(n); break;
    case 4: cfg.sync_word = (uint32_t)llround(n); break;
    case 5: cfg.payload_bytes = (uint8_t)llround(n); break;
    default: return;
    }
    /* Limits, including that the signal fits the bandwidth, are the
       demodulator's and are checked in ls_field_configure_fsk. */
    result(ls_field_configure_fsk(&cfg));
}

static void edit_fsk_setting(int i)
{
    if (i < 0 || i >= FSK_SETTING_N) return;
    s_fsk_setting = i;
    const double values[] = { s.config.freq_hz / 1e6, s.fsk.bitrate,
        s.fsk.deviation_hz, s.fsk.bandwidth_hz, s.fsk.sync_word,
        s.fsk.payload_bytes };
    static const char *const units[] = {"MHz", "baud", "Hz", "Hz",
                                        "decimal", "bytes"};
    ls_numpad_open(fsk_settings[i], units[i], values[i], set_fsk_number);
}

static void setup_fsk(void)
{
    const bool paging = s.mode == LS_LAB_POCSAG;
    const int n = paging ? 2 : FSK_SETTING_N;
    ls_picker_open(paging ? "POCSAG SETTINGS" : "GFSK SETTINGS", edit_fsk_setting);
    char value[32];
    for (int i = 0; i < n; i++) {
        switch (i) {
        case 0: snprintf(value, sizeof(value), "%.4f MHz", s.config.freq_hz / 1e6); break;
        case 1: snprintf(value, sizeof(value), "%lu baud", (unsigned long)s.fsk.bitrate); break;
        case 2: snprintf(value, sizeof(value), "%.1f kHz", s.fsk.deviation_hz / 1000.0); break;
        case 3: snprintf(value, sizeof(value), "%.1f kHz", s.fsk.bandwidth_hz / 1000.0); break;
        case 4: snprintf(value, sizeof(value), "0x%08lX", (unsigned long)s.fsk.sync_word); break;
        default: snprintf(value, sizeof(value), "%u bytes", s.fsk.payload_bytes); break;
        }
        ls_picker_add(fsk_settings[i], value);
    }
}

static void send_text(const char *text) { result(ls_field_transmit(text)); }
static void action(int i)
{
    s_feedback[0] = 0;
    if (i == 0) result(ls_field_direct(!(s.direct || s.requested)));
    else if (i == 1) result(ls_field_mode((ls_lab_mode_t)((s.mode + 1) % MODE_N)));
    else if (i == 2) { if (LS_LAB_IS_FSK(s.mode)) setup_fsk(); else setup(); }
    else if (i == 3) open_band_picker();
    else if (i == 4) {
        if (!s.direct || s.transmitting || s.mode == LS_LAB_SPECTRUM) { result(false); return; }
        ls_keyboard_open("SEND ONE LORA PACKET", "", 64, send_text);
    } else if (i == 5) result(ls_field_mark_lora());
    /* The survey log: samples.csv, one row per sample, carrying RSSI, SNR,
       frequency and packet counts alongside GPS and the IMU. It is what
       BEARING and SPECTRUM are worth keeping - not packet payloads, which
       this writer does not carry. JOURNAL has had this button all along;
       the screen actually doing the measuring did not. */
    else if (i == 6) result(ls_field_record(!s.recording));
}
static void switch_action(int i)
{
    ls_lora_cfg_t cfg = s.config;
    if (i == 0) { cfg.crc_on = !cfg.crc_on; result(ls_field_configure(&cfg)); }
    else if (i == 1) { cfg.invert_iq = !cfg.invert_iq; result(ls_field_configure(&cfg)); }
    else if (i == 2) { cfg.sync_word = cfg.sync_word == 0x34 ? 0x12 : 0x34; result(ls_field_configure(&cfg)); }
    else if (i == 3) { s_hold = !s_hold; snprintf(s_feedback, sizeof(s_feedback), "%s", s_hold ? "Plot held; compass and receiver stay live" : "Plot following live samples"); }
    else if (i == 4) { s_hold = false; result(ls_field_clear_plot()); }
    else if (i == 5) { s_guide_visible = ls_field_calibrate(0); result(s_guide_visible); ls_field_mode(LS_LAB_BEARING); }
}

static void cal_center(tui_surface *sf, tui_rect r, int row, const char *text, uint8_t attr)
{
    int x = r.x + (r.w - (int)strlen(text))/2;
    if (x < r.x) x = r.x;
    tui_put_str(sf, r, x, row, text, attr);
}

static void cal_line(tui_surface *sf, tui_rect r, int x0, int y0, int x1, int y1, uint8_t attr)
{
    int steps = abs(x1-x0) > abs(y1-y0) ? abs(x1-x0) : abs(y1-y0);
    char ch = x0 == x1 ? '|' : y0 == y1 ? '=' : (x1-x0)*(y1-y0) > 0 ? '\\' : '/';
    for (int i = 0; i <= steps; i++)
        tui_put_char(sf, r, x0 + (steps ? (x1-x0)*i/steps : 0),
                     y0 + (steps ? (y1-y0)*i/steps : 0), ch, attr);
}

static void cal_board(tui_surface *sf, tui_rect r, int step, int64_t now)
{
    if (r.w < 22 || r.h < 9) return;
    uint8_t ink = TUI_ATTR((s.calibration_aligned ? TUI_GREEN : TUI_CYAN) | TUI_BRIGHT, TUI_BLACK);
    const int cx = r.x + r.w/2, cy = r.y + r.h/2;
    if (step == LS_COMPASS_FACE_STEPS) {
        static const char *const eight[] = {
            "  .--.      .--.  ", " /    \\    /    \\ ", "|      \\  /      |",
            "|       ><       |", "|      /  \\      |", " \\    /    \\    / ", "  '--'      '--'  "};
        for (int j = 0; j < 7; j++) cal_center(sf, r, cy - 3 + j, eight[j], ink);
        cal_center(sf, r, cy - 5, "WAVE IT IN A FIGURE EIGHT", LS_ATTR_DIM);
        cal_center(sf, r, cy + 5, (now/400000)%2 ? "TURN IT OVER" : "AND AROUND", LS_ATTR_DIM);
        return;
    }
    if (step == 0 || step == 5) {
        int half = r.w/2 - 5; if (half > 13) half = 13;
        int x[] = {cx-half, cx+half-3, cx+half, cx-half+3};
        int y[] = {cy-1, cy-1, cy+2, cy+2};
        for (int i = 0; i < 4; i++) cal_line(sf, r, x[i], y[i], x[(i+1)%4], y[(i+1)%4], ink);
        cal_center(sf, r, cy, step == 0 ? "SCREEN" : "BACK OF BOARD", ink);
        cal_center(sf, r, r.y+r.h-1, "SIDE VIEW", LS_ATTR_DIM);
        cal_center(sf, r, cy-5, step == 0 ? "SCREEN FACES UP" : "SCREEN FACES DOWN", LS_ATTR_DIM);
        for (int j = 0; j < 3; j++) {
            int yarrow = step == 0 ? cy-4+j : cy+3+j;
            cal_center(sf,r,yarrow,step==0?"/|\\":"\\|/",
                (s.calibration_aligned || j == (now/220000)%3) ? ink : LS_ATTR_FAINT);
        }
        return;
    }
    int cw=10, ch=17; ls_tui_geometry(NULL, NULL, &cw, &ch);
    if (cw < 1) cw=10;
    if (ch < 1) ch=17;
    float sy = (r.h-6)*.5f, sx = sy*ch/cw;
    if (sx > (r.w-12)*.5f) { sx=(r.w-12)*.5f; sy=sx*cw/ch; }
    const float angle=(step-1)*1.570796327f, co=cosf(angle), si=sinf(angle);
    const float px[]={-.52f,.52f,.52f,-.52f}, py[]={-1,-1,1,1};
    int x[4],y[4];
    for (int i=0;i<4;i++) { x[i]=cx+(int)lroundf(sx*(px[i]*co-py[i]*si)); y[i]=cy+(int)lroundf(sy*(px[i]*si+py[i]*co)); }
    for (int i=0;i<4;i++) cal_line(sf,r,x[i],y[i],x[(i+1)%4],y[(i+1)%4],ink);
    for (int i=0;i<4;i++) tui_put_char(sf,r,x[i],y[i],'+',ink);
    cal_center(sf,r,cy,"SCREEN",ink);
    const int tx=cx+(int)lroundf(sx*.72f*si), ty=cy-(int)lroundf(sy*.72f*co);
    const int ux=cx-(int)lroundf(sx*.72f*si), uy=cy+(int)lroundf(sy*.72f*co);
    tui_put_str(sf,r,tx-1,ty,"TOP",LS_ATTR_DIM);
    tui_put_str(sf,r,ux-1,uy,"USB",TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    if(step==1) {
        static const char *const lift[]={"  /|\\"," / | \\","   |"};
        for(int j=0;j<3;j++)tui_put_str(sf,r,r.x+1,r.y+1+j,lift[j],
            (s.calibration_aligned || j==2-(now/220000)%3)?ink:LS_ATTR_FAINT);
        tui_put_str(sf,r,r.x+1,r.y+4," LIFT",LS_ATTR_DIM);
    } else {
        cal_center(sf,r,r.y,"TURN RIGHT",LS_ATTR_DIM);
        for(int j=0;j<3;j++) {
            int xarrow=cx-7+5*j;
            uint8_t attr=(s.calibration_aligned || j==(now/220000)%3)?ink:LS_ATTR_FAINT;
            tui_put_str(sf,r,xarrow,r.y+1,"==>",attr);
        }
        tui_put_str(sf,r,cx+8,r.y+2,"|",ink);
        tui_put_str(sf,r,cx+8,r.y+3,"v",ink);
    }
    cal_center(sf,r,r.y+r.h-1,"v  THIS EDGE TOWARD FLOOR  v",LS_ATTR_DIM);
}

static void calibration_action(int i)
{
    if (i == 0) { result(ls_field_calibrate(2)); s_guide_visible=false; }
    else if (i == 1) { result(ls_field_calibrate(0)); s_guide_visible=true; }
}

static void calibration_draw(tui_surface *sf, tui_rect a, int64_t now)
{
    const bool done=!s.calibrating && s.calibration_step==LS_COMPASS_STEPS && !s.calibration_failed;
    ls_btn_t buttons[]={{done?"DONE":"CANCEL",done?"COMPASS":"BACK",'q',false,false},
                       {"RESTART","GUIDE",'r',false,false}};
    int bh=ls_btn_raised_height(a,2);
    ls_btn_bar_raised(sf,tui_rect_make(a.x,a.y,a.w,bh),buttons,2,-1);
    tui_rect p=tui_rect_make(a.x,a.y+bh,a.w,a.h-bh-2);
    ls_panel_box(sf,p,"COMPASS SETUP",TUI_CYAN);
    tui_rect inside=tui_rect_make(p.x+2,p.y+1,p.w-4,p.h-2);
    const uint8_t green=TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK);
    const uint8_t yellow=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK);
    if (done || s.calibration_failed) {
        int y=inside.y+inside.h/2-3;
        cal_center(sf,inside,y,done?"[ OK ] CALIBRATION COMPLETE":"[ ! ] LET'S TRY AGAIN",done?green:yellow);
        cal_center(sf,inside,y+2,done?(s.calibration_saved?"Saved on SD card":"Active now; not saved on SD yet."):s.compass_status,LS_ATTR_DIM);
        cal_center(sf,inside,y+4,done?"Turn the screen up and hold it flat.":"Move away from metal, then tap RESTART.",LS_ATTR_DIM);
        cal_center(sf,inside,y+6,done?"Tap DONE to use the compass.":"Your previous calibration is still kept.",LS_ATTR_DIM);
    } else {
        static const char *const names[]={"SCREEN UP","USB EDGE DOWN","RIGHT EDGE DOWN","TOP EDGE DOWN","LEFT EDGE DOWN","SCREEN DOWN","FIGURE EIGHT"};
        static const char *const move[]={"Lay it flat, with the screen facing UP.","Stand it upright, like holding a phone.","Turn right; lower the RIGHT edge.","Keep turning until the TOP edge is down.","Keep turning until the LEFT edge is down.","Turn it over, with the screen facing DOWN.","Wave it slowly in a big figure eight."};
        static const char *const detail[]={"Hold it in your hand, away from metal.","The screen faces you; USB edge is down.","The USB edge now points to your left.","The USB edge now points up.","The USB edge now points to your right.","Hold 2 seconds, then turn it back over.","Tip and turn it every way as you go."};
        const unsigned steps=LS_COMPASS_STEPS;
        unsigned step=s.calibration_step<steps?s.calibration_step:steps-1;
        const bool eight=step==LS_COMPASS_FACE_STEPS;
        char line[96]; snprintf(line,sizeof(line),"%u / %u   %s",step+1,steps,names[step]);
        cal_center(sf,inside,inside.y,line,yellow);
        tui_rect picture, words;
        if (inside.w >= 76) {
            picture=tui_rect_make(inside.x,inside.y+2,34,inside.h-3);
            words=tui_rect_make(inside.x+36,inside.y+3,inside.w-36,inside.h-4);
        } else {
            int h=inside.h-12; if(h>22)h=22; if(h<0)h=0;
            picture=tui_rect_make(inside.x,inside.y+2,inside.w,h);
            words=tui_rect_make(inside.x,picture.y+picture.h+1,inside.w,inside.h-h-3);
        }
        cal_board(sf,picture,(int)step,now);
        cal_center(sf,words,words.y,move[step],LS_ATTR_DIM);
        cal_center(sf,words,words.y+2,detail[step],LS_ATTR_DIM);
        if (!s.sample.imu_valid || !s.sample.imu.mag_valid) snprintf(line,sizeof(line),"Waiting for the motion sensor...");
        else if (eight) snprintf(line,sizeof(line),"DIRECTIONS  %u of %u",
                                 (unsigned)s.calibration_cover,(unsigned)LS_COMPASS_COVER_NEEDED);
        else if (s.calibration_hold) snprintf(line,sizeof(line),"HOLD STILL  %.1f seconds",(LS_COMPASS_HOLD_SAMPLES-s.calibration_hold)/10.0);
        else snprintf(line,sizeof(line),"%s",s.calibration_aligned?"Position matched. Hold still.":"Move the board to match the picture.");
        cal_center(sf,words,words.y+4,line,(s.calibration_aligned||eight)?green:yellow);
        char bar[23]="[--------------------]";
        const int filled=eight?s.calibration_cover*20/LS_COMPASS_COVER_NEEDED:s.calibration_hold;
        for(int i=0;i<filled && i<20;i++)bar[i+1]='#';
        cal_center(sf,words,words.y+5,bar,green);
        cal_center(sf,words,words.y+7,"Next step is automatic. No tapping needed.",LS_ATTR_DIM);
        if(words.h>=22) {
            cal_center(sf,words,words.y+10,"YOUR PROGRESS",LS_ATTR_DIM);
            for(unsigned i=0;i<steps;i++) {
                snprintf(line,sizeof(line),"%u [%s] %-16s",i+1,i<step?"OK":i==step?">>":"  ",names[i]);
                cal_center(sf,words,words.y+12+i,line,i<step?green:i==step?yellow:LS_ATTR_DIM);
            }
        } else {
            char progress[60]; int n=0;
            for(unsigned i=0;i<steps;i++)n+=snprintf(progress+n,sizeof(progress)-n,"%s%u:%s",i?"  ":"",i+1,i<step?"OK":i==step?">>":"--");
            cal_center(sf,words,words.y+9,progress,LS_ATTR_DIM);
        }
    }
    ls_safe_line(sf,a,a.y+a.h-2,s.direct?"Mesh paused: DIRECT is still on":"Mesh stays active during calibration",LS_ATTR_DIM);
    ls_safe_line(sf,a,a.y+a.h-1,"Q / ESC cancel    R restart",LS_ATTR_DIM);
}
static void graph(tui_surface *sf, tui_rect r, const float *values)
{
    if (r.w < 4 || r.h < 3) return;
    for (int y = 0; y < r.h; y++) {
        for (int x = 0; x < r.w; x++) {
            if (y == r.h - 1 || (y % 4 == 0 && x % 4 == 0))
                tui_put_char(sf, r, r.x + x, r.y + y, '.', LS_ATTR_FAINT);
        }
    }
    for (int x = 0; x < r.w; x++) {
        float v = (values[x * LS_FIELD_BINS / r.w] + 140) / 110;
        if (!isfinite(v)) continue;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        int height = (int)(v * (r.h - 1));
        for (int y = 0; y < height; y++)
            tui_put_char(sf, r, r.x + x, r.y + r.h - 1 - y, '.',
                         TUI_ATTR(TUI_CYAN, TUI_BLACK));
        tui_put_char(sf, r, r.x + x, r.y + r.h - 1 - height, '_', TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    }
}
static void polar(tui_surface *sf, tui_rect r)
{
    if (r.w < 8 || r.h < 5) return;
    const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    if (cw < 1) cw = 10;
    if (ch < 1) ch = 17;
    float ry = (r.h - 2) * .45f, rx = ry * ch / cw;
    if (rx > (r.w - 4) * .5f) { rx = (r.w - 4) * .5f; ry = rx * cw / ch; }
    if (s_full_compass) {
        float t = fminf(1, (esp_timer_get_time() - s_full_open_us) / 350000.0f);
        float scale = 1 - .15f * (1-t)*(1-t)*(1-t);
        rx *= scale; ry *= scale;
    }
    const bool valid = s.sample.imu_valid && s.sample.imu.mag_valid && isfinite(s_heading);
    const float heading = valid ? s_heading : 0;
    for (int i = 0; i < 72; i++) {
        float angle = ((float)i * 5 - heading) * .0174532925f;
        int x = cx + (int)(sinf(angle) * rx), y = cy - (int)(cosf(angle) * ry);
        tui_put_char(sf, r, x, y, i % 6 ? '.' : '+', valid ? LS_ATTR_DIM : LS_ATTR_FAINT);
        if (!valid || (s_full_compass && !s_show_signal) || i % 2 || !s_bearing_count[i/2]) continue;
        float v = (s_bearing[i/2] + 140) / 110;
        if (v < .1f) v = .1f;
        if (v > 1) v = 1;
        x = cx + (int)(sinf(angle) * rx * v); y = cy - (int)(cosf(angle) * ry * v);
        tui_put_char(sf, r, x, y, '*', TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    }
    if (valid) {
        static const char cardinals[] = "NESW";
        for (int i = 0; i < 4; i++) {
            float angle = (i * 90 - heading) * .0174532925f;
            int x = cx + (int)lroundf(sinf(angle) * rx * .72f);
            int y = cy - (int)lroundf(cosf(angle) * ry * .72f);
            tui_put_char(sf, r, x, y, cardinals[i],
                         TUI_ATTR((i ? TUI_WHITE : TUI_RED) | TUI_BRIGHT, TUI_BLACK));
        }
        if (s_full_compass && r.h >= 22 && r.w >= 34) {
            for (int degrees = 0; degrees < 360; degrees += 30) {
                float angle = (degrees-heading) * .0174532925f;
                char label[4]; snprintf(label,sizeof(label),"%d",degrees);
                int x = cx + (int)lroundf(sinf(angle)*rx*.9f) - (int)strlen(label)/2;
                int y = cy - (int)lroundf(cosf(angle)*ry*.9f);
                tui_put_str(sf,r,x,y,label,LS_ATTR_DIM);
            }
        }
    }
    int marker_y = s_full_compass ? cy - (int)lroundf(ry) - 2 : r.y;
    tui_put_char(sf, r, cx, marker_y, '^', TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    tui_put_char(sf, r, cx, marker_y + 1, '|', TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    tui_put_char(sf, r, cx, cy, '+', LS_ATTR_DIM);
    if (!valid) tui_put_str(sf, r, cx - 4, cy + 1, "NO MAG", LS_ATTR_DIM);
}
static void full_compass_draw(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf,a,"COMPASS / MAGNETIC",TUI_CYAN);
    const bool wide = a.w > a.h * 2;
    tui_rect dial = tui_rect_make(a.x+2,a.y+2,wide ? a.w/2-3 : a.w-4,
                                  wide ? a.h-10 : a.h-21);
    int cw=10,ch=17; ls_tui_geometry(NULL,NULL,&cw,&ch);
    if (!wide && ch>0 && dial.h > dial.w*cw/ch+2) dial.h=dial.w*cw/ch+2;
    if (dial.h < 8) dial.h = 8;
    polar(sf,dial);
    tui_rect detail = wide ? tui_rect_make(a.x+a.w/2,a.y+3,a.w/2-2,a.h-11) :
                             tui_rect_make(a.x+2,dial.y+dial.h+1,a.w-4,12);
    char line[100];
    if (!wide && detail.w>=24 && s.sample.imu_valid && isfinite(s.sample.heading)) {
        static const uint8_t digits[10][5] = {
            {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
            {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};
        unsigned h=(unsigned)lroundf(s.sample.heading)%360;
        unsigned values[]={h/100,(h/10)%10,h%10};
        int left=detail.x+(detail.w-22)/2;
        for(int digit=0;digit<3;digit++) for(int y=0;y<5;y++) for(int x=0;x<3;x++)
            if(digits[values[digit]][y] & (4>>x)) for(int px=0;px<2;px++)
                tui_put_char(sf,a,left+digit*8+x*2+px,detail.y+y,'#',
                             TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
        detail.y+=6;
    }
    static const char *const dirs[]={"N","NE","E","SE","S","SW","W","NW"};
    if (s.sample.imu_valid && s.sample.imu.mag_valid && isfinite(s.sample.heading))
        snprintf(line,sizeof(line),"[ %03u  %s ]  MAGNETIC",
                 (unsigned)lroundf(s.sample.heading)%360,
                 dirs[(unsigned)((s.sample.heading+22.5f)/45)%8]);
    else snprintf(line,sizeof(line),"[ --- ]  WAITING FOR HEADING");
    cal_center(sf,detail,detail.y,line,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    cal_center(sf,detail,detail.y+1,s.sample.imu_valid && s.sample.imu.mag_valid ? "IMU live <350ms / orientation held" : "IMU stale or magnetometer unavailable",TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    cal_center(sf,detail,detail.y+2,"Board top ^ / N magnetic / deg CW",LS_ATTR_DIM);
    snprintf(line,sizeof(line),ls_tui_is_wide()?"%s: %s / K calibrate":"%s: %s",s.calibration_keyboard?"KEYBOARD":"STANDALONE",
             s.calibrated?(s.calibration_saved?"saved":"RAM only"):"UNCALIBRATED");
    cal_center(sf,detail,detail.y+3,line,TUI_ATTR((s.calibrated?TUI_GREEN:TUI_YELLOW)|TUI_BRIGHT,TUI_BLACK));
    if (s_show_signal) {
        cal_center(sf,detail,detail.y+4,"RECEIVED SIGNALS / latest 2",TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        for(int i=0;i<2;i++) {
            if(i<s.detection_count) {
                const ls_field_detection_t *d=&s.detections[i];
                long age=(long)((esp_timer_get_time()-d->observed_us)/1000000);
                snprintf(line,sizeof(line),"%s %.3f MHz %+.0f dBm %lds",d->mesh?"MESH":"LORA",
                         d->frequency/1e6,d->rssi,age<0?0:age);
            } else snprintf(line,sizeof(line),"%s",i ? "" : "Waiting for a received packet");
            cal_center(sf,detail,detail.y+5+i,line,LS_ATTR_DIM);
        }
        cal_center(sf,detail,detail.y+7,"HEARD MESH PEERS / may be relayed",TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        for(int i=0;i<3;i++) {
            if(i<s.peer_count)snprintf(line,sizeof(line),"%.19s  %.8s  %+.0f dBm",
                                      s.peers[i].name,s.peers[i].id,s.peers[i].rssi);
            else snprintf(line,sizeof(line),"%s",i ? "" : "No advertised peers heard");
            cal_center(sf,detail,detail.y+8+i,line,LS_ATTR_DIM);
        }
    } else {
    const ls_imu_sample_t *imu=&s.sample.imu;
    if (s.sample.imu_valid && imu->mag_valid)
        snprintf(line,sizeof(line),"FIELD %.1f uT  |  TURN %+.1f dps",
                 sqrtf(imu->mx*imu->mx+imu->my*imu->my+imu->mz*imu->mz),imu->gz);
    else snprintf(line,sizeof(line),"Magnetometer: waiting for live data");
    cal_center(sf,detail,detail.y+4,line,LS_ATTR_DIM);
    if (s.sample.imu_valid)
        snprintf(line,sizeof(line),"TILT %.0f deg  |  SENSOR %.1f C",
                 atan2f(hypotf(imu->ax,imu->ay),fabsf(imu->az))*57.29578f,imu->temp_c);
    else snprintf(line,sizeof(line),"Motion sensor: waiting for live data");
    cal_center(sf,detail,detail.y+5,line,LS_ATTR_DIM);
    if (s.sample.gps_valid) snprintf(line,sizeof(line),"GPS %.5f  %.5f",s.sample.lat,s.sample.lon);
    else snprintf(line,sizeof(line),"GPS: waiting for a fix");
    cal_center(sf,detail,detail.y+7,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    if (s.sample.gps_valid) snprintf(line,sizeof(line),"ALT %.1f m",s.sample.alt_m);
    else snprintf(line,sizeof(line),"Hold flat for heading");
    cal_center(sf,detail,detail.y+8,line,LS_ATTR_DIM);
    cal_center(sf,detail,detail.y+10,s.calibrated ? "Calibration retained" : "Calibration required",LS_ATTR_DIM);
    }
    cal_center(sf,detail,detail.y+11,s_show_signal ? "Cyan: board heading at RX, not location" : "Signal overlay hidden",LS_ATTR_DIM);
    if (s_embedded) return;
    ls_btn_t buttons[]={{"BACK","LABS",'v',false,false},{"VIEW",s_show_signal?"SIGNALS":"SENSORS",'b',s_show_signal,false},{"MARK","JOURNAL",'j',false,false},{"CAL","SETUP",'k',false,false}};
    ls_btn_bar_raised(sf,tui_rect_make(a.x+3,a.y+a.h-6,a.w-6,4),buttons,4,button_focus);
}

/* COMPASS's SIMPLE style is this screen's full compass, drawn by this
   code, without this screen's own buttons. */
void ls_scr_labs_classic_compass(tui_surface *sf, tui_rect a)
{
    ls_field_snapshot(&s);
    const int64_t now = esp_timer_get_time();
    if (now < s.sample.time_us || now - s.sample.time_us > 350000) s.sample.imu_valid = false;
    const float dt = s_heading_us ? (now - s_heading_us) / 1e6f : 1;
    s_heading = ls_compass_ease(s_heading, s.sample.imu_valid && s.sample.imu.mag_valid ? s.sample.heading : NAN, 1 - expf(-dt / .12f));
    s_heading_us = now;
    memcpy(s_bearing, s.bearing, sizeof(s_bearing)); memcpy(s_bearing_count, s.bearing_count, sizeof(s_bearing_count));
    const bool was = s_full_compass;
    s_full_compass = true; s_embedded = true;
    full_compass_draw(sf, a);
    s_full_compass = was; s_embedded = false;
}
/* Inside the frame, never on it: these notes vary in length with the mode. */
static void panel_note(tui_surface *sf, tui_rect panel, int row, const char *text)
{
    const int w = panel.w - 4;
    if (w < 1) return;
    tui_put_str(sf, tui_rect_make(panel.x + 2, row, w, 1),
                panel.x + 2, row, text, LS_ATTR_DIM);
}

static void draw(tui_surface *sf, tui_rect a)
{
    if (a.h < 14 || a.w < 24) {
        ls_panel_box(sf, a, "LORA LABS", TUI_CYAN);
        tui_put_str(sf, a, a.x + 1, a.y + 1, "Enlarge the pane", LS_ATTR_DIM);
        return;
    }
    ls_field_snapshot(&s);
    s_expand_hit = tui_rect_make(0,0,0,0);
    const int64_t now = esp_timer_get_time();
    if (s.calibrating || s_guide_visible) { calibration_draw(sf,a,now); return; }
    if(now<s.sample.time_us || now-s.sample.time_us>350000) s.sample.imu_valid=false;
    float dt = s_heading_us ? (now - s_heading_us) / 1e6f : 1;
    s_heading = ls_compass_ease(s_heading, s.sample.imu_valid && s.sample.imu.mag_valid ? s.sample.heading : NAN, 1 - expf(-dt / .12f));
    s_heading_us = now;
    if (!s_hold) {
        memcpy(s_trace, s.trace, sizeof(s_trace)); memcpy(s_spectrum, s.spectrum, sizeof(s_spectrum));
        memcpy(s_bearing, s.bearing, sizeof(s_bearing)); memcpy(s_bearing_count, s.bearing_count, sizeof(s_bearing_count));
    }
    if (s_full_compass) { full_compass_draw(sf,a); return; }
    ls_btn_t buttons[] = {{"DIRECT", s.direct ? "ON" : s.requested ? "WAIT" : "OFF", 'd', s.direct, false},
        {"MODE", modes[s.mode], 'm', false, false}, {"SETUP", NULL, 's', false, false},
        {"BAND", band_now(), 'b', false, false},
        {"SEND", s.transmitting ? "BUSY" : "ONCE", 't', s.transmitting, !s.direct || s.mode == LS_LAB_SPECTRUM},
        {"MARK", "JOURNAL", 'j', false, false},
        {"REC", rec_face(), 'r', s.recording, !s.ready}};
    const int bar_h = ls_btn_raised_height(a, 7);
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y, a.w, bar_h), buttons, 7, button_slot==0?button_focus:-1);
    ls_btn_t switches[] = {{"CRC", s.config.crc_on ? "ON" : "OFF", 'c', s.config.crc_on, s.transmitting},
        {"IQ", s.config.invert_iq ? "INVERT" : "NORMAL", 'i', s.config.invert_iq, s.transmitting},
        {"SYNC", s.config.sync_word == 0x12 ? "PRIVATE" : s.config.sync_word == 0x34 ? "PUBLIC" : "CUSTOM", 'p', s.config.sync_word == 0x34, s.transmitting},
        {"HOLD", s_hold ? "ON" : "OFF", 'h', s_hold, false}, {"CLEAR", "PLOT", 'x', false, false},
        {"CAL", s.calibrating ? "FINISH" : s.calibrated ? "AGAIN" : "COMPASS", 'k', s.calibrating, !s.sample.imu_valid}};
    tui_rect controls = tui_rect_make(a.x, a.y + bar_h, a.w, a.h - bar_h);
    if (controls.h > 40) controls.h = 40;
    const int switch_h = ls_btn_raised_height(controls, 6);
    controls.h = switch_h;
    ls_btn_bar_raised_slot(sf, controls, switches, 6, button_slot==1?button_focus:-1, LS_BTN_SLOT_WATERFALL);
    const int top = bar_h + switch_h;
    tui_rect panel = tui_rect_make(a.x, a.y + top, a.w, a.h - top - 2);
    ls_panel_box(sf, panel, "LORA LABS", TUI_CYAN);
    ls_motion_busy(sf,panel,s.busy || s.transmitting || (s.requested && !s.direct));
    char line[100];
    /* Spreading factor and coding rate are LoRa's and mean nothing to the FSK
       demodulator, so an FSK mode reports what it is actually listening
       with. Showing SF7 while running GFSK is a quiet lie about the radio. */
    if (s.mode == LS_LAB_POCSAG)
        /* Deviation, bandwidth and sync are pinned for paging, so only the
           two that a user can move are worth the width. */
        snprintf(line, sizeof(line), "%.4f MHz  %lu bd  POCSAG%s",
                 s.config.freq_hz / 1e6, (unsigned long)s.fsk.bitrate,
                 s.config.invert_iq ? "  INVERTED" : "");
    else if (s.mode == LS_LAB_GFSK)
        snprintf(line, sizeof(line), "%.4f MHz  %lu bd  dev %.1fk  bw %.1fk%s",
                 s.config.freq_hz / 1e6, (unsigned long)s.fsk.bitrate,
                 s.fsk.deviation_hz / 1000.0, s.fsk.bandwidth_hz / 1000.0,
                 s.config.invert_iq ? "  INV" : "");
    else
        snprintf(line, sizeof(line), "%.4f MHz  SF%u  %.1fk  4/%u", s.config.freq_hz / 1e6, s.config.sf, s.config.bw_hz / 1000.0, s.config.cr);
    tui_put_str(sf, panel, panel.x + 2, panel.y + 1, line, LS_ATTR_DIM);
    snprintf(line, sizeof(line), "%s | RX %lu BAD %lu TX %lu", s.direct || s.busy ? "MESH PAUSED" : "MESH CONTROL", (unsigned long)s.rx, (unsigned long)s.bad, (unsigned long)s.tx);
    uint8_t fresh=ls_fresh(&packet_arrival,s.rx+s.tx+s.bad,600);
    tui_put_str(sf, panel, panel.x + 2, panel.y + 2, line,
                ls_fresh_attr(fresh,TUI_WHITE|TUI_BRIGHT,(s.direct || s.busy ? TUI_YELLOW : TUI_GREEN)|TUI_BRIGHT,TUI_BLACK));
    int plot_h = panel.h - 9;
    if (plot_h > 18) plot_h = 18;
    tui_rect plot = tui_rect_make(panel.x + 2, panel.y + 4, panel.w - 4, plot_h);
    if (s.mode == LS_LAB_BEARING) polar(sf, plot);
    else if(s.mode==LS_LAB_SPECTRUM) {
        bool fresh=s.direct && s.spectrum_us>0 && now>=s.spectrum_us && now-s.spectrum_us<3000000;
        if(fresh) {
            graph(sf,plot,s_spectrum);
            int peak=0;for(int i=1;i<LS_FIELD_BINS;i++)if(s_spectrum[i]>s_spectrum[peak])peak=i;
            snprintf(line,sizeof(line),"Peak %.1f dBm / bin %d of %d",s_spectrum[peak],peak+1,LS_FIELD_BINS);
            tui_put_str(sf,plot,plot.x,plot.y,line,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
        }
        else tui_put_str(sf,plot,plot.x+1,plot.y+1,s.direct?"No complete fresh sweep":"Enable DIRECT to scan; Mesh will pause",TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
        snprintf(line,sizeof(line),"%.3f <- MHz -> %.3f | -140..-30 dBm",(s.config.freq_hz-1000000)/1e6,(s.config.freq_hz+1000000)/1e6);
        tui_put_str(sf,plot,plot.x,plot.y+plot.h-1,line,LS_ATTR_DIM);
    } else if (s.mode == LS_LAB_POCSAG) pages_list(sf, plot);
    else graph(sf, plot, s_trace);
    const char *caption = s.mode == LS_LAB_BEARING ? "Yellow: board top | Red N: north" :
        s.mode == LS_LAB_SPECTRUM ? "Swept energy: brief packets may fall between looks" :
        s.mode == LS_LAB_POCSAG ? "UP/DOWN walk the pages | newest first" :
        s.direct ? "6.4 seconds | channel RSSI -140..-30 dBm" : "Last mesh packet RSSI | -140..-30 dBm";
    panel_note(sf, panel, plot.y + plot.h + 1, caption);
    if (s.mode == LS_LAB_BEARING) {
        static const char *const directions[] = {"N","NE","E","SE","S","SW","W","NW"};
        if (isfinite(s.sample.heading)) snprintf(line, sizeof(line), "[%03u %s MAG] %s", (unsigned)lroundf(s.sample.heading) % 360,
            directions[(unsigned)((s.sample.heading + 22.5f) / 45) % 8],
            s.calibrated ? s.calibration_saved ? "calibration saved on SD" : "calibration in RAM" : "CALIBRATION REQUIRED");
        else snprintf(line, sizeof(line), "Waiting for a valid magnetic reading");
        tui_put_str(sf, panel, panel.x + 2, plot.y + plot.h + 2, line, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }
    if(s.mode==LS_LAB_SPECTRUM) {
        snprintf(line,sizeof(line),"SX1262 sweeps %lu / errors %lu / age %s",(unsigned long)s.spectrum_sweeps,(unsigned long)s.spectrum_errors,s.direct&&s.spectrum_us&&now>=s.spectrum_us&&now-s.spectrum_us<3000000?"<3s":"STALE");
        tui_put_str(sf,panel,panel.x+2,plot.y+plot.h+2,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    }
    if (panel.h > 36) {
        tui_rect info = tui_rect_make(panel.x + 1, plot.y + plot.h + 3, panel.w - 2, panel.h - plot.h - 11);
        ls_panel_box(sf, info, "LIVE CONTEXT", TUI_CYAN);
        snprintf(line, sizeof(line), "%s", s.direct ? "DIRECT / mesh paused" : "MESH / passive view");
        ls_kv(sf, info, 2, "OWNER", line, LS_ATTR_DIM);
        snprintf(line, sizeof(line), "%.1f dBm", s.trace[LS_FIELD_BINS - 1]);
        ls_kv(sf, info, 4, "RSSI", line, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        snprintf(line, sizeof(line), "%s", s.sample.gps_valid ? "fresh fix" : "waiting for fix");
        ls_kv(sf, info, 6, "GPS", line, LS_ATTR_DIM);
        if (s.sample.imu_valid && s.sample.imu.mag_valid && isfinite(s.sample.heading)) snprintf(line, sizeof(line), "%.0f magnetic", s.sample.heading);
        else snprintf(line, sizeof(line), "unavailable");
        ls_kv(sf, info, 8, "HEADING", line, LS_ATTR_DIM);
        snprintf(line, sizeof(line), "%s", s.config.crc_on ? "CRC on" : "CRC off");
        ls_kv(sf, info, 10, "PACKETS", line, LS_ATTR_DIM);
        ls_kv(sf, info, 12, "JOURNAL", "MARK attaches this observation", LS_ATTR_DIM);
    }
    if (s.mode == LS_LAB_POCSAG) {
        /* The list above carries the messages; this is the running total,
           which is what tells you the decoder is alive on a quiet channel. */
        snprintf(line, sizeof(line), "%lu page%s decoded this session",
                 (unsigned long)s.pages, s.pages == 1 ? "" : "s");
        panel_note(sf, panel, panel.y + panel.h - 3, line);
    } else if (s.mode == LS_LAB_PACKETS || s.mode == LS_LAB_GFSK) {
        int n = s.packet_len < 12 ? s.packet_len : 12, used = 0;
        for (int i = 0; i < n; i++) used += snprintf(line + used, sizeof(line) - used, "%02X ", s.packet[i]);
        if (!n) snprintf(line, sizeof(line), "%s", s.mode == LS_LAB_GFSK
            ? "Waiting for an FSK frame with this sync word"
            : "Waiting for a matching LoRa packet");
        panel_note(sf, panel, panel.y + panel.h - 3, line);
    }
    ls_safe_line(sf, a, a.y + a.h - 2, s.status, LS_ATTR_DIM);
    ls_safe_line(sf, a, a.y + a.h - 1, s.calibrating && s.compass_status[0] ? s.compass_status : s_feedback[0] ? s_feedback : "Leaving Labs returns the radio to Mesh", LS_ATTR_DIM);
    if (s.mode == LS_LAB_BEARING) {
        s_expand_hit=tui_rect_make(a.x+3,a.y+a.h-6,a.w-6,4);
        ls_btn_t expand={"EXPAND","COMPASS",'v',false,false};
        ls_btn_bar_raised_slot(sf,s_expand_hit,&expand,1,-1,LS_BTN_SLOT_QUICK);
    }
}
/* COMPASS asks for the calibration guide and then opens this screen. */
static bool s_calibrate_on_enter;
void ls_scr_labs_request_calibration(void) { s_calibrate_on_enter = true; }
static void enter_guide(void) { if (s_calibrate_on_enter) { s_calibrate_on_enter = false; s_guide_visible = ls_field_calibrate(0); } }
static void enter(void) { expand_compass(false); button_focus=-1;button_slot=0; s_guide_visible = false; s_hold = false; s_heading = NAN; s_heading_us = 0; s_feedback[0] = 0; if (!ls_field_start()) snprintf(s_feedback, sizeof(s_feedback), "Field worker could not start"); ls_field_watch(true); enter_guide(); }
static void leave(void) { s_guide_visible = false; ls_field_direct(false); ls_field_calibrate(2); ls_field_watch(false); }
static bool touch(int col, int row) {
    int i = ls_btn_hit(col,row);
    if (s.calibrating || s_guide_visible) { if(i>=0)calibration_action(i); return true; }
    if (s_full_compass) { if(i==0)expand_compass(false);else if(i==1)s_show_signal=!s_show_signal;else if(i==2)action(5);else if(i==3){expand_compass(false);switch_action(5);}return true; }
    if (col>=s_expand_hit.x && col<s_expand_hit.x+s_expand_hit.w &&
        row>=s_expand_hit.y && row<s_expand_hit.y+s_expand_hit.h) { expand_compass(true);return true; }
    if(i>=0)action(i);else { i=ls_btn_hit_slot(col,row,LS_BTN_SLOT_WATERFALL);if(i>=0)switch_action(i); }
    return true;
}
static bool key(ls_tk_t k, char ch) {
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (s.calibrating || s_guide_visible) {
        if(k==LS_TK_ESC || k==LS_TK_BACKSPACE || (k==LS_TK_CHAR && ch=='q')) calibration_action(0);
        else if(k==LS_TK_CHAR && ch=='r') calibration_action(1);
        return k==LS_TK_CHAR || k==LS_TK_ESC || k==LS_TK_BACKSPACE;
    }
    if (s_full_compass) {
        if (k==LS_TK_CHAR && ch=='k') { expand_compass(false);switch_action(5);return true; }
        if (k==LS_TK_ESC || k==LS_TK_BACKSPACE || (k==LS_TK_CHAR && (ch=='v' || ch=='q'))) { expand_compass(false);return true; }
        if (k==LS_TK_CHAR && ch=='j') { action(5);return true; }
        if (k==LS_TK_CHAR && ch=='b') { s_show_signal=!s_show_signal;return true; }
        if (ls_btn_navigate(k,&button_slot,&button_focus,false)) return true;
        if (k==LS_TK_ENTER) { if(button_focus==0)expand_compass(false);else if(button_focus==1)s_show_signal=!s_show_signal;else if(button_focus==2)action(5);else if(button_focus==3){expand_compass(false);switch_action(5);}return true; }
        return false;
    }
    if (k==LS_TK_CHAR && ch=='v') { expand_compass(true);return true; }
    /* The page list owns UP/DOWN while it is the view, the way the FM pager
       does. Buttons stay reachable by letter and by thumb. */
    if (s.mode == LS_LAB_POCSAG && (k == LS_TK_UP || k == LS_TK_DOWN)) {
        const int count = s.page_log_count;
        s_page_sel += k == LS_TK_DOWN ? 1 : -1;
        if (s_page_sel >= count) s_page_sel = count ? count - 1 : 0;
        if (s_page_sel < 0) s_page_sel = 0;
        return true;
    }
    if (ls_btn_navigate(k,&button_slot,&button_focus,true)) return true;
    if (k==LS_TK_ENTER) { if(ls_btn_enabled(button_slot,button_focus)) { if(button_slot==0)action(button_focus);else switch_action(button_focus); } return true; }
    if (k != LS_TK_CHAR || !ch) return false;
    const char *p = strchr("dmsbtjr", ch); if (p) { action((int)(p - "dmsbtjr")); return true; }
    p = strchr("ciphxk", ch); if (!p) return false; switch_action((int)(p - "ciphxk")); return true;
}
const ls_tui_screen_t ls_scr_labs = {.name="LORA LABS", .hint="D direct  M mode  S setup  B band  R record  K calibrate  H hold", .enter=enter, .leave=leave, .draw=draw, .key=key, .touch=touch, .hold_auto_rotation=true};
