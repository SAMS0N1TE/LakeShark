/* Capture level and receiver-rate history.
   Use the nonblocking hub status; probe storage only on entry.
   Keep line charts so steady rates do not fill the plot. */
#include "../../ls_tui_screen.h"

#include "esp_timer.h"
#include "esp_attr.h"
#include "../../ls_motion.h"
#include "../../ls_rec_replay.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../ls_tui.h"
#include "../../ls_tui_ui.h"
#include "apps/rec/rec_state.h"
#include "apps/rec/rec_watch.h"
#include "../../ls_picker.h"
#include "../../ls_field.h"
#include "core/ls_time.h"
#include "core/ls_track_log.h"

extern const ls_tui_screen_t ls_scr_subghz;
extern const ls_tui_screen_t ls_scr_rec;
extern const ls_tui_screen_t ls_scr_gps;
extern const ls_tui_screen_t ls_scr_journal;
extern void ls_scr_journal_rec_view(void);
static bool tools_view, gps_view;
static int recorder_source;
typedef struct { const char *name, *format; ls_field_source_t field; } recorder_source_t;
/* One inventory for selection and labels. Metadata is not raw signal capture. */
static const recorder_source_t recorder_sources_table[] = {
    {"RTL-SDR", "OOK pulse captures", LS_FIELD_RTL},
    {"CC1101", "OOK pulse captures", LS_FIELD_CC1101},
    {"GPS TRACK", "Position track to SD", LS_FIELD_NONE},
    {"HackRF", "CSV receiver metadata", LS_FIELD_HACKRF},
    {"SX1262 / Mesh", "CSV radio metadata", LS_FIELD_MESH},
    {"nRF24", "CSV survey metadata", LS_FIELD_NRF24},
    {"NFC", "CSV field metadata", LS_FIELD_NFC},
    {"Wi-Fi", "CSV link metadata", LS_FIELD_WIFI},
    {"Bluetooth", "CSV link metadata", LS_FIELD_BLE},
};
#define RECORDER_SOURCE_COUNT ((int)(sizeof(recorder_sources_table)/sizeof(recorder_sources_table[0])))
static tui_rect source_bar;
static char recorder_hint[80];

static uint64_t s_free_bytes;
static bool     s_free_known;

/* Sample at 320 ms intervals; reset across missing data or retunes. */
#define HIST      128
#define SAMPLE_US 320000

static uint16_t s_mag[HIST];
static uint16_t s_thresh[HIST];
static int s_level_top = 64;
static int s_scale_quiet;
static uint16_t s_rate[HIST];
static int      s_head = -1;      /* -1 until the first sample */
static int      s_filled;
static int64_t s_last_sample;
static uint32_t s_freq;

/* Blink the recording indicator while capturing. */
static uint32_t s_blink;

static void on_enter(void)
{
    /* Filesystem call, once, off the draw path. */
    s_free_bytes = rec_dir_free_bytes();
    s_free_known = true;

    s_head = -1;
    s_filled = 0;
    s_last_sample = 0;
    s_level_top = 64;
    s_scale_quiet = 0;
}

static bool sample(const rec_hub_status_t *st)
{
    const int64_t now = esp_timer_get_time();
    if (!st->receiver_streaming || st->freq_hz != s_freq ||
        (s_filled && now - s_last_sample > SAMPLE_US * 2)) {
        s_head = -1;
        s_filled = 0;
        s_last_sample = 0;
        s_level_top = 64;
        s_scale_quiet = 0;
    }
    s_freq = st->freq_hz;
    if (!st->receiver_streaming) return false;
    if (s_filled && now - s_last_sample < SAMPLE_US) return false;
    s_last_sample = now;
    s_head = (s_head + 1) % HIST;
    s_mag[s_head]  = (uint16_t)(st->mag_now > 65535 ? 65535 :
                                st->mag_now < 0 ? 0 : st->mag_now);
    /* Keep threshold and scale on the same clock as the trace. */
    s_thresh[s_head] = (uint16_t)(st->mag_thresh > 65535 ? 65535 :
                                 st->mag_thresh < 0 ? 0 : st->mag_thresh);
    /* Rate in KiB/s, so the ring stays 16 bit up to 64 MB/s - far past what
       an SD card on this bus will ever take. */
    s_rate[s_head] = (uint16_t)(st->bytes_sec / 1024u);
    if (s_filled < HIST) s_filled++;
    return true;
}

static void row(tui_surface *sf, tui_rect a, int y, const char *label,
                const char *value, uint8_t va)
{
    tui_put_str(sf, a, a.x + 2, a.y + y, label,
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, a, a.x + 14, a.y + y, value, va);
}

static const char *phase_name(rec_phase_t p)
{
    switch (p) {
    case REC_IDLE:      return "idle";
    case REC_ARMED:     return "armed";
    case REC_CAPTURING: return "CAPTURING";
    case REC_DONE:      return "done";
    default:            return "?";
    }
}

/* Newest sample at right; leave missing columns empty.
   Draw the threshold before the trace and label the scale at the call site. */
static void chart(tui_surface *sf, tui_rect a, int x, int y, int w, int h,
                  const uint16_t *ring, const uint16_t *thresholds,
                  int lo, int top, int mark,
                  uint8_t trace_at, uint8_t hot_at, uint8_t mark_at)
{
    const int span = top - lo;
    if (w < 4 || h < 2 || span <= 0) return;

    /* The threshold first, so the trace draws over it where they meet. */
    if (mark >= lo && mark <= top) {
        const int my = y + h - 1 - ((mark - lo) * (h - 1) / span);
        for (int i = 0; i < w; i++)
            tui_put_char(sf, a, x + i, my, (i & 1) ? '-' : ' ', mark_at);
    }

    if (s_head < 0) return;

    int prev_cell = -1;
    for (int i = 0; i < w; i++) {
        const int back = w - 1 - i;
        if (back >= s_filled) { prev_cell = -1; continue; }
        const int idx = (s_head - back + HIST * 2) % HIST;

        int v = ring[idx];
        if (v > top) v = top;
        if (v < lo)  v = lo;

        const int eighths = (v - lo) * (h * 8) / span;
        int cell = eighths / 8;
        int rem  = eighths % 8;
        if (cell >= h) { cell = h - 1; rem = 8; }
        if (!rem) rem = 1;      /* a sample at the baseline is still a sample */

        /* Historical colors use the threshold at acquisition. */
        const int threshold = thresholds ? thresholds[idx] : mark;
        const uint8_t at = (threshold > 0 && ring[idx] >= threshold)
                           ? hot_at : trace_at;

        /* Join it to the sample before it.

           Without this the chart is a scatter of single marks and the eye
           has to do the joining, which it does badly when the gaps are
           large - a burst forty cells above the floor read as two unrelated
           specks. The run is drawn first and the sample's own glyph over
           it, so the endpoint keeps its sub-cell position. */
        if (prev_cell >= 0 && prev_cell != cell) {
            const int from = (prev_cell < cell) ? prev_cell + 1 : cell + 1;
            const int to   = (prev_cell < cell) ? cell : prev_cell;
            for (int c = from; c <= to; c++)
                tui_put_char(sf, a, x + i, y + h - 1 - c, '|', at);
        }

        tui_put_char(sf, a, x + i, y + h - 1 - cell, LS_TUI_TRACE(rem), at);
        prev_cell = cell;
    }
}

/* The range the samples actually occupy.

   Both ends, because scaling from zero is what turned a steady write rate
   into a filled rectangle. A trace of 497..503 against a 0..503 scale is a
   flat line at the top; against 495..505 it is the shape it really has. The
   baseline is only lifted when the samples sit well clear of zero, so a
   chart of something that genuinely idles at nothing still shows that. */
static void ring_range(const uint16_t *ring, int *out_lo, int *out_top)
{
    int lo = 65535, hi = 0;
    for (int i = 0; i < s_filled; i++) {
        if (ring[i] < lo) lo = ring[i];
        if (ring[i] > hi) hi = ring[i];
    }
    if (s_filled == 0) { lo = 0; hi = 0; }

    int base = 0;
    if (lo > hi / 4 && hi > 0) {
        const int pad = (hi - lo) / 4 + 1;
        base = lo - pad;
        if (base < 0) base = 0;
    }
    *out_lo  = base;
    *out_top = hi;
}

static void human_bytes(char *buf, size_t len, uint64_t n)
{
    if (n >= 1024ull * 1024 * 1024) snprintf(buf, len, "%.1f GB", (double)n / (1024.0 * 1024 * 1024));
    else if (n >= 1024ull * 1024)   snprintf(buf, len, "%.1f MB", (double)n / (1024.0 * 1024));
    else if (n >= 1024ull)          snprintf(buf, len, "%.1f kB", (double)n / 1024.0);
    else                            snprintf(buf, len, "%u B", (unsigned)n);
}

/* ------------------------------------------------------------------ parts */

static void draw_facts(tui_surface *sf, tui_rect a, const rec_hub_status_t *st)
{
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t hot  = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim  = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    char buf[48];

    row(sf, a, 1, "state", phase_name(st->phase),
        st->phase == REC_CAPTURING ? hot : st->phase == REC_ARMED ? val : dim);
    /* The recording light. One cell, immediately before the word it
       is redundant with as text and not redundant with as motion - see the
       note by s_blink. Drawn after the row itself so it sits on top of
       whatever blank column CAPTURING's left edge leaves there. */
    if (st->phase == REC_CAPTURING && ((s_blink / 12) & 1))
        tui_put_char(sf, a, a.x + 13, a.y + 1, LS_TUI_SHADE_FULL, hot);

    snprintf(buf, sizeof(buf), "%.4f MHz", (double)st->freq_hz / 1e6);
    row(sf, a, 2, "frequency", buf, val);

    row(sf, a, 3, "receiver", st->receiver_streaming ? "streaming" : "stopped",
        st->receiver_streaming ? good : dim);

    snprintf(buf, sizeof(buf), "%u", (unsigned)st->captures);
    row(sf, a, 4, "captures", buf, st->captures ? good : dim);

    if (a.h > 6) {
        snprintf(buf, sizeof(buf), "%d", st->edges);
        row(sf, a, 5, "edges", buf, st->edges ? val : dim);
    }

    if (a.h > 7 && s_free_known) {

        const bool known = (s_free_bytes != UINT64_MAX);
        if (known) human_bytes(buf, sizeof(buf), s_free_bytes);
        else       snprintf(buf, sizeof(buf), "no card");
        row(sf, a, 6, "free", buf,
            !known                               ? hot
            : s_free_bytes < 32ull * 1024 * 1024 ? hot : dim);
    }
}

/* Shrink only after ten samples fit a smaller visible range. */
static void level_scale(int width)
{
    int peak = 0;
    for (int back = 0; back < s_filled && back < width; back++) {
        const int i = (s_head - back + HIST) % HIST;
        if (s_mag[i] > peak) peak = s_mag[i];
        if (s_thresh[i] > peak) peak = s_thresh[i];
    }
    peak += peak / 4;
    int want = 64;
    while (want < peak && want < 65536) want *= 2;
    if (want > s_level_top || (want < s_level_top && ++s_scale_quiet >= 10)) {
        s_level_top = want;
        s_scale_quiet = 0;
    } else if (want == s_level_top) {
        s_scale_quiet = 0;
    }
}

static void draw_level(tui_surface *sf, tui_rect a, const rec_hub_status_t *st,
                       bool sampled)
{
    const uint8_t dim = LS_ATTR_DIM;
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t hot  = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t on   = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    char buf[48];

    ls_panel_box(sf, a, "LEVEL", TUI_CYAN);
    if (a.h < 7 || a.w < 24) return;

    if (sampled) level_scale(a.w - 2);
    /* A zero baseline and delayed scale changes keep the trace readable. */
    const int lo = 0, top = s_level_top;
    const int level = s_head >= 0 ? s_mag[s_head] : 0;
    const int threshold = s_head >= 0 ? s_thresh[s_head] : 0;

    const int h = a.h - 5;
    chart(sf, a, a.x + 1, a.y + 1, a.w - 2, h, s_mag, s_thresh, lo, top,
          threshold, on, hot, val);

    snprintf(buf, sizeof(buf), "level %d  trigger %d",
             level, threshold);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 4, buf,
                (threshold > 0 && level >= threshold) ? hot : dim);
    snprintf(buf, sizeof(buf), "scale %d-%d raw; yellow = trigger", lo, top);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 3, buf, dim);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 2,
                !st->receiver_streaming ? "RX stopped: no live level" :
                st->phase == REC_ARMED ? "Armed: waiting for trigger" :
                st->phase == REC_CAPTURING ? "Capturing: red >= trigger" :
                "Preview: press ARM to capture", dim);
}

static void draw_rate(tui_surface *sf, tui_rect a, const rec_hub_status_t *st)
{
    const uint8_t dim = LS_ATTR_DIM;
    const uint8_t on  = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);

    ls_panel_box(sf, a, "RX RATE", TUI_CYAN);
    if (a.h < 7 || a.w < 24) return;

    int lo, top;
    ring_range(s_rate, &lo, &top);
    if (top < lo + 8) top = lo + 8;

    const int h = a.h - 5;
    chart(sf, a, a.x + 1, a.y + 1, a.w - 2, h, s_rate, NULL, lo, top, -1,
          on, on, dim);

    const int now = (s_head >= 0) ? s_rate[s_head] : 0;
    char cap[80];
    snprintf(cap, sizeof(cap), "%d KiB/s  auto %d-%d", now, lo, top);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 4, cap, dim);
    const int visible = s_filled < a.w - 2 ? s_filled : a.w - 2;
    snprintf(cap, sizeof(cap), "%.1fs ago <-- time --> now",
             visible > 1 ? (visible - 1) * (SAMPLE_US / 1000000.0) : 0.0);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 3, cap, dim);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 2,
                !st->receiver_streaming ? "RX stopped: no live samples" :
                !st->bytes_sec ? "Waiting for receiver samples" :
                "Receiver input, not disk writes", dim);
}

/* ------------------------------------------------------------------- draw */

#define REC_BTN_H 5

static tui_rect s_btn;

/* Whether a CAPTURE is running, which is not what rec_active() says. */

static bool capture_running(void)
{
    rec_hub_status_t st;
    memset(&st, 0, sizeof(st));
    rec_get_hub_status(&st);
    return st.phase == REC_ARMED || st.phase == REC_CAPTURING;
}

static void capture_toggle(void)
{
    if (capture_running()) rec_disarm();
    else                   rec_arm_request();
}

static void draw_button(tui_surface *sf, tui_rect a, bool active)
{
    s_btn = a;
    if (a.w < 8 || a.h < 3) { s_btn.w = 0; return; }

    const uint8_t hue = active ? (TUI_GREEN | TUI_BRIGHT) : TUI_CYAN;
    ls_panel_box(sf, a, NULL, hue);

    tui_rect f = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    if (f.w <= 0 || f.h <= 0) return;
    ls_fill_dither(sf, f, active ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);
    ls_dither_label(sf, f, (f.h - 1) / 2, active ? "STOP" : "ARM",
                    TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

static void draw(tui_surface *sf, tui_rect area)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    char buf[48];

    rec_hub_status_t st;
    memset(&st, 0, sizeof(st));
    rec_get_hub_status(&st);
    const bool sampled = sample(&st);
    char stamp[LS_TIME_STAMP_MAX];
    if(st.captures || st.phase==REC_CAPTURING)
        ls_time_render_stamp_at(stamp,sizeof(stamp),0,(int64_t)st.capture_uptime_s*1000000);
    else snprintf(stamp,sizeof(stamp),"-- / no capture");
    snprintf(buf,sizeof(buf),"CAPTURE START %s",stamp);
    tui_put_str(sf,area,area.x,area.y,buf,dim);
    area.y++;area.h--;
    s_blink++;

    tui_box(sf, area, "CAPTURE", frame);

    tui_rect body = tui_rect_make(area.x + 1, area.y + 1,
                                  area.w - 2, area.h - 2);
    if (body.h < 6 || body.w < 14) return;

    const int foot = (body.h > 10) ? 1 : 0;
    int usable = body.h - foot;

    s_btn.w = 0;
    if (usable >= REC_BTN_H + 8) {
        draw_button(sf, tui_rect_make(body.x, body.y + usable - REC_BTN_H,
                                      body.w, REC_BTN_H),
                    st.phase == REC_ARMED || st.phase == REC_CAPTURING);
        usable -= REC_BTN_H;
    }

    if (ls_tui_is_wide()) {
        /* Landscape has width and not height: facts on the left, the two
           charts stacked on the right, both short. */
        tui_rect left, right;
        ls_tui_split(tui_rect_make(body.x, body.y, body.w, usable),
                     &left, &right);
        draw_facts(sf, left, &st);
        const int lh = (right.h * 3) / 5;
        draw_level(sf, tui_rect_make(right.x, right.y, right.w, lh), &st, sampled);
        if (right.h - lh >= 4)
            draw_rate(sf, tui_rect_make(right.x, right.y + lh, right.w,
                                        right.h - lh), &st);
    } else {
        /* Portrait has the height. The facts take what they need and the
           charts divide the rest, level getting the larger share because it
           is the one being read against a threshold. */
        const int facts_h = (usable > 22) ? 8 : 7;
        draw_facts(sf, tui_rect_make(body.x, body.y, body.w, facts_h), &st);

        const int rest = usable - facts_h;
        if (rest >= 8) {
            const int lh = (rest * 3) / 5;
            draw_level(sf, tui_rect_make(body.x, body.y + facts_h,
                                         body.w, lh), &st, sampled);
            draw_rate(sf, tui_rect_make(body.x, body.y + facts_h + lh,
                                        body.w, rest - lh), &st);
        } else if (rest >= 5) {
            draw_level(sf, tui_rect_make(body.x, body.y + facts_h,
                                         body.w, rest), &st, sampled);
        }
    }

    if (foot) {
        const char *d = rec_dir();
        snprintf(buf, sizeof(buf), "to %s", d ? d : "?");
        tui_put_str(sf, area, area.x + 2, area.y + area.h - 2, buf, dim);
    }
}

static bool key(ls_tk_t k, char ch)
{
    (void)ch;
    if (k == LS_TK_ENTER) {

        capture_toggle();
        return true;
    }
    return false;
}

/* The button, and nothing else. A tap anywhere else in this screen
   is claimed and ignored - see the note above draw_button for what it used
   to do instead. */
static bool touch(int col, int row)
{
    if (s_btn.w > 0 &&
        col >= s_btn.x && col < s_btn.x + s_btn.w &&
        row >= s_btn.y && row < s_btn.y + s_btn.h) {
        capture_toggle();
    }
    return true;
}

/* File replay is a workspace, not a picker that sends on selection. */
#define REPLAY_PREVIEW 128
static EXT_RAM_BSS_ATTR struct {
    subghz_file_t file;
    int32_t edges[REPLAY_PREVIEW];
    char path[160];
    char result[112];
    uint64_t preview_us;
    int n, selected, power;
    bool loaded, waiting;
    int64_t started, finished;
} player;
static bool replay_view;
static bool rec_child_active;
static tui_rect rec_tabs[3], replay_trace;
static ls_fresh_t replay_flash;
static const int replay_ook_power[]={-10,0,5,10};
static const int replay_fsk_power[]={-9,0,14,22};
static int replay_dbm(void) {return subghz_file_is_ook(&player.file)?replay_ook_power[player.power]:replay_fsk_power[player.power];}
static void replay_poll(void)
{
    if(!player.waiting)return;
    char result[112];
    if(rec_watch_replay_status(result,sizeof(result)))return;
    player.waiting=false;player.finished=esp_timer_get_time();
    snprintf(player.result,sizeof(player.result),"%s",result[0]?result:"Replay ended without a result");
    ls_fresh_bump(&replay_flash);
}
static void replay_play(void)
{
    replay_poll();
    if(!player.loaded || player.waiting)return;
    if(!rec_watch_request_replay_file(player.path,replay_dbm())) {
        snprintf(player.result,sizeof(player.result),"Radio busy - stop WATCH / SCAN first");return;
    }
    player.waiting=true;player.started=esp_timer_get_time();player.finished=0;
    snprintf(player.result,sizeof(player.result),"Queued - waiting for radio worker");
}
static void replay_browse(void)
{
    for(int i=0;i<ls_tui_screen_count();i++)if(!strcmp(ls_tui_screen_name(i),"FILES")){ls_tui_screen_show(i);break;}
}
static void replay_control(int i)
{
    if(i==0)replay_play();
    else if(i==1 && !player.waiting && player.power>0)player.power--;
    else if(i==2 && !player.waiting && player.power<3)player.power++;
    else if(i==3 && !player.waiting)replay_browse();
}
static void replay_draw(tui_surface *sf,tui_rect a)
{
    replay_poll();replay_trace=tui_rect_make(0,0,0,0);
    if(!player.loaded) {
        ls_panel_notice(sf,a,"REPLAY","FILES > capture > ACTIONS > REPLAY","Load a file, then press PLAY ONCE");return;
    }
    char power[24];snprintf(power,sizeof(power),"%+d dBm",replay_dbm());
    ls_btn_t controls[]={
        {player.waiting?"BUSY":"PLAY ONCE",player.waiting?"WAIT":"TRANSMIT",'p',player.waiting,player.waiting},
        {"POWER-",power,'-',false,player.waiting || player.power==0},
        {"POWER+",power,'+',false,player.waiting || player.power==3},
        {"BROWSE","FILES",'o',false,player.waiting}};
    int h=ls_tui_is_wide()?5:10;
    if(a.h<h+12)h=5;
    tui_rect bar=tui_rect_make(a.x,a.y,a.w,h);
    if(!ls_tui_is_wide() && bar.w>38){bar.x+=(bar.w-38)/2;bar.w=38;}
    ls_btn_bar_raised_slot(sf,bar,controls,4,-1,LS_BTN_SLOT_QUICK);
    a.y+=h;a.h-=h;
    const bool ook=subghz_file_is_ook(&player.file);
    ls_panel_box(sf,a,ook?"REPLAY / CC1101 OOK":"REPLAY / SX1262 FSK",TUI_CYAN);
    ls_motion_busy(sf,a,player.waiting);
    const uint8_t ink=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK);
    const char *name=strrchr(player.path,'/');name=name?name+1:player.path;
    char text[112];
    tui_put_str(sf,a,a.x+2,a.y+1,name,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
    snprintf(text,sizeof(text),"%.4f MHz  |  %s  |  %+.0f dBm nominal",player.file.freq_hz/1e6,ook?"RAW OOK":"RAW FSK",(double)replay_dbm());
    tui_put_str(sf,a,a.x+2,a.y+2,text,ink);
    snprintf(text,sizeof(text),"%.2f ms  |  %d edges  |  one transmission",player.file.span_us/1000.,player.file.edges);
    tui_put_str(sf,a,a.x+2,a.y+3,text,LS_ATTR_DIM);
    if(a.h>=15 && player.n && player.preview_us) {
        replay_trace=tui_rect_make(a.x+3,a.y+6,a.w-6,a.h-12);
        if(replay_trace.h>12)replay_trace.h=12;
        tui_put_str(sf,a,a.x+2,a.y+5,"STORED PULSES: HIGH / LOW",ink);
        /* This sweep indicates worker activity, not measured RF position.
           Pulse geometry stays fixed while the palette and cursor move. */
        const int sweep_ms=350;
        const int trail=replay_trace.w/6>4?replay_trace.w/6:4;
        int head=-1;
        const int64_t now=esp_timer_get_time();
        if(player.waiting || player.finished) {
            int64_t elapsed=(now-player.started)/1000;
            if(elapsed<0)elapsed=0;
            /* Completion keeps this cycle's position, then lets the same
               blue trail leave the right edge. Never restart a green pass. */
            int64_t position=elapsed%sweep_ms;
            if(!player.waiting) {
                int64_t at_finish=(player.finished-player.started)/1000;
                if(at_finish<0)at_finish=0;
                position=at_finish%sweep_ms+(now-player.finished)/1000;
            }
            if(position>=0 && position*replay_trace.w/sweep_ms<replay_trace.w+trail)
                head=(int)(position*replay_trace.w/sweep_ms);
        }
        uint64_t sum=0;int edge=0,prev=-1,head_y=replay_trace.y;
        for(int x=0;x<replay_trace.w;x++) {
            uint64_t t=(uint64_t)x*player.preview_us/replay_trace.w;
            while(edge<player.n-1 && sum+(uint64_t)(player.edges[edge]<0?-(int64_t)player.edges[edge]:player.edges[edge])<=t) {
                sum+=player.edges[edge]<0?-(int64_t)player.edges[edge]:player.edges[edge];edge++;
            }
            int y=player.edges[edge]>0?replay_trace.y:replay_trace.y+replay_trace.h-1;
            uint8_t fg=edge==player.selected?(TUI_YELLOW|TUI_BRIGHT):TUI_GREEN;
            int distance=head-x;
            if(head>=0 && distance>=0 && distance<=trail) {
                fg=distance<trail/3?(TUI_CYAN|TUI_BRIGHT):distance<2*trail/3?(TUI_BLUE|TUI_BRIGHT):TUI_BLUE;
                if(distance==0)fg=TUI_WHITE|TUI_BRIGHT;
            }
            if(x==head)head_y=y;
            uint8_t color=TUI_ATTR(fg,TUI_BLACK);
            if(prev>=0 && prev!=y)for(int k=replay_trace.y;k<replay_trace.y+replay_trace.h;k++)tui_put_char(sf,a,replay_trace.x+x,k,'|',color);
            tui_put_char(sf,a,replay_trace.x+x,y,'-',color);prev=y;
        }
        if(head>=0 && head<replay_trace.w) {
            uint8_t glow_ink=TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK);
            for(int y=replay_trace.y;y<replay_trace.y+replay_trace.h;y++)
                tui_put_char(sf,a,replay_trace.x+head,y,y==head_y?'+':':',glow_ink);
        }
        snprintf(text,sizeof(text),"0 -> %.2f ms  (first %d/%d edges)",player.preview_us/1000.,player.n,player.file.edges);
        tui_put_str(sf,a,a.x+2,replay_trace.y+replay_trace.h,text,LS_ATTR_DIM);
        int32_t v=player.edges[player.selected];
        snprintf(text,sizeof(text),"Tap trace: #%d %s %lu us",player.selected+1,v>0?"HIGH":"LOW",(unsigned long)(v<0?-(int64_t)v:v));
        tui_put_str(sf,a,a.x+2,replay_trace.y+replay_trace.h+1,text,ink);
    }
    if(player.waiting)snprintf(text,sizeof(text),"%c Working %.1fs - one bounded send",ls_motion_pip(true),(esp_timer_get_time()-player.started)/1e6);
    else snprintf(text,sizeof(text),"%s",player.result[0]?player.result:"READY - press PLAY ONCE to transmit");
    uint8_t level=ls_fresh_level(&replay_flash,1500);
    uint8_t result_color=player.waiting || !player.result[0]?TUI_CYAN:
                         !strncmp(player.result,"Sent",4)?TUI_GREEN:TUI_RED;
    tui_put_str(sf,a,a.x+2,a.y+a.h-3,text,ls_fresh_attr(level,TUI_WHITE,result_color,TUI_BLACK));
    tui_put_str(sf,a,a.x+2,a.y+a.h-2,player.waiting?"White line = activity, not RF progress":"Yellow = selected pulse; stored preview",LS_ATTR_DIM);
}
static bool replay_key(ls_tk_t k,char c)
{
    if(k==LS_TK_ENTER || c=='p'||c=='P'){replay_control(0);return true;}
    if(c=='-'){replay_control(1);return true;}
    if(c=='+' || c=='='){replay_control(2);return true;}
    if(c=='o'||c=='O'){replay_control(3);return true;}
    if(k==LS_TK_LEFT && player.selected>0){player.selected--;return true;}
    if(k==LS_TK_RIGHT && player.selected+1<player.n){player.selected++;return true;}
    return false;
}
static bool replay_touch(int x,int y)
{
    int b=ls_btn_hit_slot(x,y,LS_BTN_SLOT_QUICK);
    if(b>=0){replay_control(b);return true;}
    if(tui_rect_contains(replay_trace,x,y) && player.preview_us) {
        uint64_t t=(uint64_t)(x-replay_trace.x)*player.preview_us/replay_trace.w,sum=0;
        for(int i=0;i<player.n;i++) {sum+=player.edges[i]<0?-(int64_t)player.edges[i]:player.edges[i];if(sum>t){player.selected=i;break;}}
    }
    return true;
}

static const ls_tui_screen_t *recorder_child(void) { return recorder_source >= 3 ? &ls_scr_journal : gps_view ? &ls_scr_gps : &ls_scr_subghz; }
static void recorder_source_done(int choice)
{
    if(choice < 0 || choice >= RECORDER_SOURCE_COUNT || rec_watch_enabled() || ls_field_recording() || ls_track_rec_running()) return;
    if(choice >= 3 && (!ls_field_start() || !ls_field_source(recorder_sources_table[choice].field))) return;
    const ls_tui_screen_t *old = recorder_child();
    if(old->leave) old->leave();
    recorder_source = choice;
    gps_view = choice == 2;
    tools_view = false;
    if(choice < 2) rec_watch_select_source(choice == 1 ? REC_SOURCE_CC1101 : REC_SOURCE_RTL);
    const ls_tui_screen_t *next = recorder_child();
    if(next->enter) next->enter();
    if(choice>=3) ls_scr_journal_rec_view();
}
static void recorder_sources(void)
{
    if(rec_watch_enabled() || ls_field_recording() || ls_track_rec_running()) return;
    ls_picker_open("RECORDING SOURCE",recorder_source_done);
    for(int i=0;i<RECORDER_SOURCE_COUNT;i++)
        ls_picker_add(recorder_sources_table[i].name,
                      i < 2 ? "OOK pulses" : i == 2 ? "GPS points" : "CSV metadata");
}
static void recorder_enter(void) { tools_view=false; replay_poll(); if(replay_view){rec_child_active=false;return;} rec_child_active=true; const ls_tui_screen_t *c=recorder_child(); if(c->enter)c->enter(); if(recorder_source>=3)ls_scr_journal_rec_view(); }
static void recorder_leave(void) { if(rec_child_active){const ls_tui_screen_t *c=recorder_child(); if(c->leave)c->leave();}rec_child_active=false; }
static void recorder_mode(bool replay)
{
    if(replay_view==replay)return;
    recorder_leave();replay_view=replay;recorder_enter();
}
bool ls_scr_rec_replay_file(const char *path,const subghz_file_t *f,const int32_t *edges)
{
    replay_poll();
    if(player.waiting || !path || strlen(path)>=sizeof(player.path) || !f || !edges ||
       (!subghz_file_is_ook(f) && !subghz_file_is_fsk(f)))return false;
    int i=ls_tui_screen_index_of(&ls_scr_rec);if(i<0)return false;
    snprintf(player.path,sizeof(player.path),"%s",path);player.file=*f;
    player.n=f->edges<REPLAY_PREVIEW?f->edges:REPLAY_PREVIEW;
    player.preview_us=0;player.selected=0;player.power=0;player.result[0]=0;player.finished=0;
    for(int n=0;n<player.n;n++){player.edges[n]=edges[n];player.preview_us+=edges[n]<0?-(int64_t)edges[n]:edges[n];}
    player.loaded=true;
    if(ls_tui_screen_current()==i)recorder_mode(true);
    else {replay_view=true;ls_tui_screen_show(i);}
    ls_fresh_bump(&replay_flash);return true;
}
static void recorder_draw(tui_surface *sf,tui_rect a) {
    if(a.w<24 || a.h<12) { source_bar=tui_rect_make(0,0,0,0); ls_panel_notice(sf,a,"REC","Enlarge pane",""); return; }
    if(!tools_view) {
    const int tab_h=ls_tui_is_wide()?3:5;
    const char *tabs[]={"RECORD","REPLAY","FILES"};
    for(int i=0;i<3;i++) {
        int left=a.x+i*a.w/3,right=a.x+(i+1)*a.w/3;
        rec_tabs[i]=tui_rect_make(left,a.y,right-left-1,tab_h);
        ls_panel_box(sf,rec_tabs[i],NULL,(i==0&&!replay_view)||(i==1&&replay_view)?TUI_GREEN:TUI_CYAN);
        tui_put_str(sf,rec_tabs[i],left+2,a.y+tab_h/2,tabs[i],TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    }
    a.y+=tab_h;a.h-=tab_h;
    } else memset(rec_tabs,0,sizeof(rec_tabs));
    if(replay_view){snprintf(recorder_hint,sizeof(recorder_hint),"P play once  +/- power  O files  LEFT/RIGHT inspect");replay_draw(sf,a);return;}
    snprintf(recorder_hint,sizeof(recorder_hint),"%s",tools_view?"B recorder  ENTER arm/stop":recorder_source>=3?"U source  C CSV metadata  V sensors":gps_view?"U source  R GPS track  M map":"U source  W watch  E export  D RTL tools");
    int h=ls_tui_is_wide()?3:5;
    if(tools_view) {
        source_bar=tui_rect_make(0,0,0,0);
        ls_btn_t back={"RECORDER","BACK",'b',false,false};
        ls_btn_bar_raised(sf,tui_rect_make(a.x,a.y,a.w,h),&back,1,-1);
        a.y+=h;a.h-=h;draw(sf,a);return;
    }
    ls_btn_t source={"SOURCE",recorder_sources_table[recorder_source].name,'u',false,rec_watch_enabled() || ls_field_recording() || ls_track_rec_running()};
    source_bar=tui_rect_make(a.x,a.y,a.w>56?24:a.w/2,h);
    ls_panel_box(sf,source_bar,NULL,source.dim?TUI_WHITE:TUI_CYAN);
    char source_label[40];
    if(h==3) {
        snprintf(source_label,sizeof(source_label),"SOURCE %s",source.value);
        tui_put_str(sf,source_bar,source_bar.x+2,source_bar.y+1,source_label,LS_ATTR_DIM);
    } else {
        tui_put_str(sf,source_bar,source_bar.x+2,source_bar.y+1,"SOURCE",LS_ATTR_DIM);
        tui_put_str(sf,source_bar,source_bar.x+2,source_bar.y+2,source.value,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    }
    tui_rect info=tui_rect_make(a.x+source_bar.w+1,a.y,a.w-source_bar.w-1,h);
    tui_put_str(sf,info,info.x,info.y,recorder_sources_table[recorder_source].format,LS_ATTR_DIM);
    char stamp[LS_TIME_STAMP_MAX];
    ls_time_render_stamp(stamp,sizeof(stamp));
    if(info.w<20 && strlen(stamp)==20) {
        stamp[10]=0;
        tui_put_str(sf,info,info.x,info.y+1,stamp,LS_ATTR_DIM);
        tui_put_str(sf,info,info.x,info.y+2,stamp+11,LS_ATTR_DIM);
    } else tui_put_str(sf,info,info.x,info.y+1,stamp,LS_ATTR_DIM);
    if(h>3) tui_put_str(sf,info,info.x,info.y+3,"CLOCK",LS_ATTR_DIM);
    a.y+=h; a.h-=h;
    recorder_child()->draw(sf,a);
}
static bool recorder_key(ls_tk_t k,char c) {
    if(k==LS_TK_TAB){recorder_mode(!replay_view);return true;}
    if(replay_view)return replay_key(k,c);
    if(k==LS_TK_CHAR && (c=='u'||c=='U')){recorder_sources();return true;}
    if(!tools_view)return recorder_child()->key(k,c);
    if(k==LS_TK_ESC || c=='b'||c=='B'){tools_view=false;return true;}
    return key(k,c);
}
static bool recorder_touch(int x,int y) {
    for(int i=0;i<3;i++)if(tui_rect_contains(rec_tabs[i],x,y)) {
        if(i==2)replay_browse();else recorder_mode(i==1);return true;
    }
    if(replay_view)return replay_touch(x,y);
    if(x>=source_bar.x && x<source_bar.x+source_bar.w && y>=source_bar.y && y<source_bar.y+source_bar.h){recorder_sources();return true;}
    if(!tools_view)return recorder_child()->touch(x,y);
    if(ls_btn_hit(x,y)==0){tools_view=false;return true;}
    return touch(x,y);
}
void ls_scr_rec_tools(void) {
    replay_view=false;
    int index=ls_tui_screen_index_of(&ls_scr_rec);
    if(index>=0)ls_tui_screen_show(index);
    recorder_source=0;gps_view=false;tools_view=true;ls_tui_radio_want("REC");on_enter();
}

const ls_tui_screen_t ls_scr_rec = {
    /* Opening the universal hub must not seize the SDR from another app. */
    .radio = NULL,
    .name = "REC",
    .hint = recorder_hint,
    .enter = recorder_enter,
    .leave = recorder_leave,
    .draw = recorder_draw,
    .key = recorder_key,
    .touch = recorder_touch,
};
