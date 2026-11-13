#include "ls_board.h"
#include "../../ls_tui_screen.h"
#include "../../ls_scan_look.h"
#include "../../ls_tui_ui.h"
#include "../../ls_numpad.h"
#include "../../ls_picker.h"
#include "../../ls_app.h"
#include "../../ls_freqs.h"
#include "../../ls_radio_select.h"
#include "../../ls_options.h"
#include "../../ls_motion.h"
#include "../../ls_field.h"
#include "rec_state.h"
#include "rec_watch.h"
#include "ls_mesh.h"
#include "ls_mixrf.h"
#include "../../ls_waterfall.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#include "../../ls_wf_source.h"
#include "../../ls_tui_touch.h"
#include "../../ls_drag_scroll.h"
#include "../../ls_icons.h"
#include "../../ls_rec_replay.h"
#include "../../ls_catch_ui.h"
#include "../../ls_radio_panel.h"
#include "settings.h"
#include "ls_lora.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "ls_textpack.h"

static int button_focus=-1, button_slot;
/* Defined below, beside the other source helpers, but the banner draws
   above them. */
static bool source_takes_the_mesh(void);
static bool hit_flash_on(void);
static EXT_RAM_BSS_ATTR rec_watch_status_t s;
static int selected;
static char feedback[80], peers[LS_MESH_MAX_PEERS][17];
static tui_rect list;
static bool touch_nav;
static tui_rect pulse_hit;
/* The two halves of the threshold control on the right edge of the
   spectrum. Set while drawing, tested on touch - the spectrum is not a
   button bar, so it carries its own hit rects. */
/* The threshold track: the full height of the plot, down its right side.
   Tapping it sets the threshold to whatever dB that height represents, so
   it reads against the same scale as the bars beside it. */
static tui_rect gate_hit;
static float gate_hit_span, gate_hit_noise;
static int pulse_selected;
static uint64_t pulse_total;
/* Two flags, because DETAILS was one and meant two unrelated things: while a
   sweep ran it chose detections over the spectrum, and the rest of the time
   it laid a legend over the capture list. Sharing a bool meant turning the
   legend off changed what the next sweep would draw, which nothing on the
   screen explained. */
static bool sweep_hits;   /* detections instead of the spectrum or the list */
/* The counter in the banner, which opens and closes the detections view. */
static tui_rect det_hit;
/* The frequency behind each drawn detection row, so a tap acts on what was
   on the screen even if the list is re-sorted before the finger lifts. */
#define DET_ROWS_MAX 32
static tui_rect det_rows;
static uint32_t det_row_hz[DET_ROWS_MAX];
static int det_row_n,det_sel,det_first,det_total;
static tui_rect det_pager;
/* A finger dragged on a list moves its window by itself and leaves the
   selection where it was; a key or a tap that moves the selection puts the
   window back on it. One pair per list: detections, captures, SAVED. */
static ls_drag_scroll_t det_drag,read_drag,saved_drag;
static bool det_free,read_free,saved_free;
static int read_first;
/* The paged lists can stand on a last page that runs past the end; a drag
   that starts there may go back from it but never jumps from it. */
static int drag_span(const ls_drag_scroll_t *d,int first,int total,int visible)
{
    const int from=d->held?d->start_first:first;
    return from+visible>total?from+visible:total;
}
static bool notes;        /* capture list: the legend overlay              */
static ls_fresh_t arrivals;
static int setup_item;
/* Sized off the enum, not off how many sources there happened to be when
   this was written. */
static uint32_t source_frequency[REC_SOURCE_COUNT]={433920000,433920000,433920000};
/* The capture source as the RADIO picker counts it, and its name there. */
static ls_rsel_radio_t source_radio(void) { return rec_source_radio(rec_watch_source()); }
static bool lr_ook(void)
{
    return rec_watch_source()==REC_SOURCE_SX1262 && ls_lora_chip()==LS_LORA_CHIP_LR20XX &&
           !rec_watch_lr_fsk();
}
static const char *source_name(void) { return ls_rsel_name(source_radio()); }

/* When WATCH last started, so the banner can show a session running rather
   than a flag that is merely set. Reset on every transition, which is also
   what makes a restart visible. */
static int64_t listening_since;

static int ord[REC_WATCH_SLOTS], ord_n, pick_pos;
static uint32_t pick_id;
static bool pick_follow = true;
static void order_build(void)
{
    ord_n = 0;
    for (int i = 0; i < s.count && i < REC_WATCH_SLOTS; i++) {
        int j = ord_n++;
        while (j > 0 && s.event[ord[j - 1]].order < s.event[i].order) { ord[j] = ord[j - 1]; j--; }
        ord[j] = i;
    }
    int pos = pick_follow ? 0 : -1;
    for (int k = 0; pos < 0 && k < ord_n; k++)
        if (s.event[ord[k]].id == pick_id) pos = k;
    if (pos < 0) pos = pick_pos;
    if (pos >= ord_n) pos = ord_n ? ord_n - 1 : 0;
    if (pos < 0) pos = 0;
    pick_pos = pos;
    selected = ord_n ? ord[pos] : 0;
    if (ord_n) pick_id = s.event[selected].id;
}
static void pick_at(int pos)
{
    if (!ord_n) return;
    read_free = false;
    if (pos < 0) pos = 0;
    if (pos >= ord_n) pos = ord_n - 1;
    pick_pos = pos;
    pick_follow = pos == 0;
    selected = ord[pos];
    pick_id = s.event[selected].id;
}
static void pick_move(int d) { pick_at(pick_pos + d); }
static bool tx_busy(void);

/* THE JOBS READ RUNS WHERE YOU ARE, rather than on pages of their own.
 *
 * LEARN and RECORD used to be tabs, which meant leaving the list of what was
 * heard to measure it or to keep a raw copy of it - and coming back to find
 * WATCH off, because both need the radio. They run from READ now: the banner
 * says which one has the radio and how far it has got, the list stays where
 * it was, and WATCH comes back when LEARN is done with the receiver.
 *
 * Declared up here because the banner, drawn first, is what reports them. */
#define LEARN_SECONDS 30
static bool learn_mine;        /* the running learn was started here        */
static bool learn_resume;      /* WATCH was on when LEARN took the radio    */
static int64_t learn_since;
static bool rec_mine;          /* RECORD is armed from here                 */
static bool rec_iq;            /* ...on the LR2021's I/Q, not REC's slicer  */
static uint32_t rec_caps0;     /* REC's capture count when it was armed     */
/* LISTEN HERE asked while a sweep held the radio: the frequency it is
   handing READ, until the sweep lets go. */
static uint32_t listen_pending;
/* The frequency changed under READ - a hand-off from BAND - so the figures
   spin to it and stay lit for a moment, the way FM's dial does on a retune. */
static ls_fresh_t handoff;
EXT_RAM_BSS_ATTR static ls_radio_glass_t glass;

/* Whether BAND is the page in front, which changes what the detections chip
   does when it is tapped. Defined with the pages. */
static bool on_band(void);
/* A one-row level meter: the live reading as a bar, REC's trigger as a mark
   on it. RECORD's banner carries it so the trigger can be judged without
   opening the full recorder view. */
static void level_meter(tui_surface *sf, tui_rect r, int x, int y, int w,
                        const rec_hub_status_t *rx)
{
    if (w < 6) return;
    int top = rx->mag_thresh * 2;
    if (rx->mag_now > top) top = rx->mag_now;
    if (top < 64) top = 64;
    const int fill = rx->mag_now > 0 ? (int)((int64_t)rx->mag_now * w / top) : 0;
    const int mark = rx->mag_thresh > 0 ? (int)((int64_t)rx->mag_thresh * w / top) : -1;
    const bool hot = rx->mag_thresh > 0 && rx->mag_now >= rx->mag_thresh;
    for (int i = 0; i < w; i++) {
        const bool lit = i < fill;
        char g = lit ? LS_TUI_BLOCK_FULL : LS_TUI_TRACE(1);
        uint8_t at = lit ? TUI_ATTR((hot ? TUI_RED : TUI_GREEN) | TUI_BRIGHT, TUI_BLACK)
                         : LS_ATTR_DIM;
        if (i == mark) { g = '|'; at = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK); }
        tui_put_char(sf, r, x + i, y, g, at);
    }
}
/* Where the banner's state chip was drawn while RECORD runs: a tap on it
   opens the full recorder view. */
static tui_rect rec_chip_hit;

/* WHAT THIS SCREEN IS DOING, before anything it can be asked to do.
 *
 * The old layout put eleven buttons at the top and the word LISTENING three
 * rows below them in dim grey, which is how you end up with a screen nobody
 * can tell is on. State goes first, in the state's own colour, with something
 * moving in it: a band that is green and counting is unambiguous from across
 * a bench in daylight, and no arrangement of buttons is.
 *
 * One line of state and one of evidence. The evidence line is what separates
 * "armed and hearing nothing" from "armed and deaf", which is the question
 * this screen exists to answer and the one the old one could not. */
static void state_banner(tui_surface *sf, tui_rect r,
                         const rec_watch_status_t *s,
                         const rec_hub_status_t *rx, int64_t now,
                         uint8_t fresh)
{
    const bool sending = tx_busy();
    const bool learning = rec_watch_learn_busy();
    const bool recording = rec_mine;
    const bool tuning = listen_pending != 0;
    /* A job that has the radio is as alive as a receiver that is streaming:
       the pip moves for both. */
    const bool live = sending || learning || recording ||
                      (s->enabled && rx->receiver_streaming);
    const bool trouble = !sending && !learning && !recording && !tuning &&
                         s->enabled && !rx->receiver_streaming;

    const char *word = sending ? "SENDING"
                     : learning ? "LEARNING"
                     : recording ? "RECORDING"
                     : tuning ? "TUNING"
                     : !s->ready ? "LOADING"
                     : !s->enabled ? "STOPPED"
                     : live ? "LISTENING"
                     : "NO RECEIVER";
    /* Each job its own colour, so which one has the radio is seen before it
       is read: magenta for measuring, red for the tape running. */
    const uint8_t hue = sending || tuning ? TUI_YELLOW
                      : learning ? TUI_MAGENTA
                      : recording ? TUI_RED
                      : !s->enabled ? TUI_CYAN
                      : trouble ? TUI_RED
                      : TUI_GREEN;

    /* NOT a dithered fill. Dither is how this interface draws a button, so
       a status strip painted that way reads as another row of controls -
       which is exactly how it was first reported: "the app now has two
       source buttons". State must not look like something you can press.

       So: one inverse chip around the state word, which is a badge and not a
       key, and plain text for everything beside it. The chip still carries
       the colour, so it is readable across a bench, and nothing about it
       invites a finger. */
    tui_fill(sf, r, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));

    char line[120];
    /* The pip only moves while something is actually arriving, so a frozen
       receiver reads as frozen instead of as idle. RECORD's is the dot a
       tape deck shows, blinking while it is armed. */
    if (recording)
        snprintf(line, sizeof(line), " %c %s ",
                 ls_motion_phase(2, 900) ? ' ' : LS_TUI_SHADE_FULL, word);
    else if (learning && learn_since) {
        int64_t left = LEARN_SECONDS - (now - learn_since) / 1000000;
        if (left < 0) left = 0;
        snprintf(line, sizeof(line), " %c %s %llds ", ls_motion_pip(true), word,
                 (long long)left);
    } else
        snprintf(line, sizeof(line), " %c %s ", ls_motion_pip(live), word);
    tui_put_str(sf, r, r.x, r.y, line, TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT));
    const int chip_w = (int)strlen(line);
    rec_chip_hit = recording ? tui_rect_make(r.x, r.y, chip_w, 2) : tui_rect_make(0, 0, 0, 0);

    /* The figures spin to a new frequency rather than jump, and stay lit
       for a moment after a hand-off - the same dial FM turns on a retune -
       so the change is seen even by someone who was looking at the list. */
    const uint32_t shown = ls_radio_glass_tick(&glass, rx->freq_hz);
    snprintf(line, sizeof(line), "%.4f MHz", shown / 1e6);
    if ((int)strlen(line) + 4 < r.w)
        tui_put_str(sf, r, r.x + r.w - 1 - (int)strlen(line), r.y, line,
                    ls_fresh_attr(ls_fresh_level(&handoff, 1500),
                                  TUI_YELLOW | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT,
                                  TUI_BLACK));

    /* Second line: elapsed, what has been heard, and whether it is on the
       card. A session that has been up for four minutes with nothing heard is
       a different situation from one that just started, and the old screen
       showed them identically. */
    if (r.h < 2) return;
    if (source_takes_the_mesh() && (s->enabled || learning) && r.w > 40 &&
        chip_w + 13 + 14 < r.w) {
        static const char *const M = "MESH PAUSED";
        tui_put_str(sf, r, r.x + chip_w + 1, r.y, M,
                    TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }
    det_hit = tui_rect_make(0, 0, 0, 0);
    /* What the job in hand is doing, in place of the session counters: the
       instruction first, because LEARN and RECORD both hear nothing until
       somebody presses the transmitter. */
    if (learning) {
        const int pct = rec_watch_scan_progress();
        const char *stage = rec_watch_scan_stage();
        const int bar = r.w >= 44 ? 10 : 0;
        snprintf(line, sizeof(line), "Press the remote");
        tui_put_str(sf, r, r.x + 1, r.y + 1, line,
                    TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
        int x = r.x + 1 + (int)strlen(line) + 2;
        const int room = r.x + r.w - 1 - (bar ? bar + 6 : 0) - x;
        /* The stage whole or not at all - "Trying 2400 ba" reads as a
           fault. "Trying " is the first thing to go on a narrow pane. */
        if (stage && stage[0]) {
            const char *say = stage;
            if ((int)strlen(say) > room && !strncmp(say, "Trying ", 7)) say += 7;
            if ((int)strlen(say) <= room)
                tui_put_str(sf, r, x, r.y + 1, say, LS_ATTR_DIM);
        }
        if (bar) {
            const int bx = r.x + r.w - 1 - bar - 5;
            const int full = bar * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100;
            for (int i = 0; i < bar; i++)
                tui_put_char(sf, r, bx + i, r.y + 1,
                             i < full ? LS_TUI_BLOCK_FULL : LS_TUI_TRACE(1),
                             TUI_ATTR((i < full ? TUI_MAGENTA : TUI_BLUE) | TUI_BRIGHT,
                                      TUI_BLACK));
            snprintf(line, sizeof(line), "%3d%%", pct);
            tui_put_str(sf, r, bx + bar + 1, r.y + 1, line,
                        TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        }
        return;
    }
    if (recording) {
        if (rec_iq)
            snprintf(line, sizeof(line), "Press the remote - up to 262 ms of I/Q");
        else if (rx->phase == REC_CAPTURING)
            snprintf(line, sizeof(line), "Capturing  %d edges", rx->edges);
        else
            snprintf(line, sizeof(line), "Press the remote");
        tui_put_str(sf, r, r.x + 1, r.y + 1, line,
                    TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
        /* The level against the trigger, which is the one thing worth
           watching while it waits. */
        const int mx = r.x + 1 + (int)strlen(line) + 2;
        const int mw = r.x + r.w - 1 - mx;
        if (!rec_iq && mw >= 8)
            level_meter(sf, r, mx, r.y + 1, mw > 20 ? 20 : mw, rx);
        return;
    }
    if (tuning) {
        snprintf(line, sizeof(line), "Handing %.4f MHz to READ", listen_pending / 1e6);
        tui_put_str(sf, r, r.x + 1, r.y + 1, line,
                    TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        return;
    }
    const uint32_t up = (s->enabled && listening_since)
                      ? (uint32_t)((now - listening_since) / 1000000) : 0;
    const int events = rec_watch_scan_events();
    if (rec_watch_scan_busy() || events) {
        /* A chip, because it is a control: it opens BAND, where the
           detections are, or on BAND swaps the list and the chart on a pane
           too small for both. It blinks with every new burst, so a count
           going up is seen rather than read. */
        static EXT_RAM_BSS_ATTR rec_scan_bin_t tmp[REC_SCAN_BINS];
        const int freqs = rec_watch_scan_result(tmp, REC_SCAN_BINS);
        snprintf(line, sizeof(line), " %d DETECTED  %d FREQ %s",
                 events, freqs > 0 ? freqs : 0, on_band() ? "" : " BAND > ");
        const bool blink = hit_flash_on();
        const uint8_t hue = events ? TUI_GREEN : TUI_CYAN;
        const uint8_t at = blink ? TUI_ATTR(TUI_BLACK, TUI_WHITE | TUI_BRIGHT)
                                 : TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT);
        tui_put_str(sf, r, r.x + 1, r.y + 1, line, at);
        det_hit = tui_rect_make(r.x, r.y, (int)strlen(line) + 2, 2);
    } else {
        LS_SNPRINTF(t_scr_subghz_lu_02lu_d_capture, line, sizeof(line), "%lu:%02lu   %d capture%s   heard %lu",
                 (unsigned long)(up / 60), (unsigned long)(up % 60),
                 s->count, s->count == 1 ? "" : "s",
                 (unsigned long)s->received);
        tui_put_str(sf, r, r.x + 1, r.y + 1, line,
                    ls_fresh_attr(fresh, TUI_WHITE | TUI_BRIGHT,
                                  TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }

    const char *store = s->save_failed ? "SD FAIL"
                      : s->pending_save ? "SAVING"
                      : s->saved ? "ON CARD" : "IN RAM";
    const uint32_t lost = s->dropped;
    if (lost) snprintf(line, sizeof(line), "%s  %lu LOST", store,
                       (unsigned long)lost);
    else      snprintf(line, sizeof(line), "%s", store);
    if ((int)strlen(line) + 4 < r.w)
        tui_put_str(sf, r, r.x + r.w - 1 - (int)strlen(line), r.y + 1, line,
                    (lost || s->save_failed)
                        ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
                        : LS_ATTR_DIM);
}
/* What the SOURCE button should read. The CC1101 is in the keyboard, so it
   can simply be absent, and rec_watch_select_source does not check - it calls
   ls_mixrf_start and returns true either way. Detection is asynchronous, so
   refusing the switch would report "missing" for a chip that is merely still
   probing. Reporting live state instead never traps you on a dead source and
   says why WATCH does nothing - a lit control that quietly does nothing is
   the fault this project already has a rule about. */
static const char *source_face(const ls_mixrf_status_t *cc)
{
    if (rec_watch_source() != REC_SOURCE_CC1101) return source_name();
    if (cc->cc) return "CC1101";
    return cc->busy ? "PROBING" : "NO CHIP";
}
/* The SX1262 is the mesh's radio the rest of the time, so a watch on it
   stops the mesh for as long as it runs. Worth saying on the screen rather
   than leaving somebody to notice their node went quiet. */
static bool source_takes_the_mesh(void)
{
    return rec_watch_source() == REC_SOURCE_SX1262;
}
static const double bands[]={152.600,154.785,315.000,433.920,868.350,915.000};
static void tune(double mhz)
{
    if(rec_watch_source()==REC_SOURCE_CC1101 && !((mhz>=300 && mhz<=348)||(mhz>=387 && mhz<=464)||(mhz>=779 && mhz<=928))) {
        LS_SNPUTS(t_scr_subghz_cc1101_300_348_387, feedback, sizeof(feedback), "CC1101: 300-348 / 387-464 / 779-928 MHz");return;
    }
    if(!isfinite(mhz) || mhz<1 || mhz>4294) {LS_SNPUTS(t_scr_subghz_frequency_outside_receiver_range, feedback, sizeof(feedback), "Frequency outside receiver range");return;}
    if(rec_watch_enabled()) {snprintf(feedback,sizeof(feedback),"Stop WATCH before changing frequency");return;}
    rec_disarm();
    if(!rec_set_freq((uint32_t)(mhz*1e6+.5)))
        LS_SNPUTS(t_scr_subghz_frequency_refused_by_selected, feedback, sizeof(feedback), "Frequency refused by selected receiver");
}
static void peer_done(int i)
{
    if(i==0)rec_watch_alert_target("");
    else if(i<=LS_MESH_MAX_PEERS && peers[i-1][0])rec_watch_alert_target(peers[i-1]);
}
static void setup_value(double value)
{
    static const double low[]={0,2,4,10,6},high[]={255,2000,10000,30000,4096};
    if(rec_watch_enabled()) {LS_SNPUTS(t_scr_subghz_stop_watch_before_changing, feedback, sizeof(feedback), "Stop WATCH before changing capture setup");return;}
    if(setup_item<0 || setup_item>4 || !isfinite(value) || value<low[setup_item] || value>high[setup_item]) {
        LS_SNPUTS(t_scr_subghz_value_outside_the_displayed, feedback, sizeof(feedback), "Value outside the displayed range");return;
    }
    rec_disarm();
    int n=(int)(value+.5);
    if(setup_item==0)rec_set_thresh(n);
    else if(setup_item==1)rec_set_gap_ms(n);
    else if(setup_item==2)rec_set_min_pulse(n);
    else if(setup_item==3)rec_set_max_span((uint32_t)n*1000);
    else rec_set_min_edges(n);
}
/* What the SX1262 listens with. The other sources time edges and have no
   modulation to set; this one demodulates, and a receiver pointed at the
   wrong rate or sync word hears nothing at all while looking exactly like
   one that is working. */
static int fsk_item;
static void fsk_value(double value)
{
    static const double low[]={600,600,5,8,0};
    static const double high[]={300000,200000,467,1024,4294967295.0};
    if(rec_watch_enabled()){snprintf(feedback,sizeof(feedback),"Stop WATCH first");return;}
    if(fsk_item<0 || fsk_item>4 || !isfinite(value) ||
       value<low[fsk_item] || value>high[fsk_item]) {
        LS_SNPUTS(t_scr_subghz_value_outside_the_displayed, feedback, sizeof(feedback), "Value outside the displayed range");return;
    }
    rec_fsk_mod_t m; rec_watch_fsk_get(&m);
    switch(fsk_item) {
    case 0: m.bitrate=(uint32_t)(value+.5); break;
    case 1: m.deviation_hz=(uint32_t)(value+.5); break;
    case 2: m.bandwidth_khz=(uint16_t)(value+.5); break;
    case 3: m.preamble_bits=(uint16_t)(value+.5); break;
    default: m.sync_word=(uint32_t)value; break;
    }
    if(!rec_watch_fsk_set(&m)) {
        LS_SNPUTS(t_scr_subghz_refused_check_the_rate, feedback, sizeof(feedback), "Refused - check the rate and deviation");
        return;
    }
    rec_watch_fsk_get(&m);
    LS_SNPRINTF(t_scr_subghz_lu_bd_1f_khz, feedback,sizeof(feedback),"%lu bd  %.1f kHz dev  %u kHz bw  sync %08lX",
        (unsigned long)m.bitrate,m.deviation_hz/1000.0,
        (unsigned)m.bandwidth_khz,(unsigned long)m.sync_word);
}
static void fsk_done(int i)
{
    if(rec_watch_enabled()){snprintf(feedback,sizeof(feedback),"Stop WATCH first");return;}
    if(i==5) {
        const rec_fsk_mod_t d={.bitrate=4800,.deviation_hz=25000,
            .sync_word=0x2DD42DD4u,.preamble_bits=32,.bandwidth_khz=59};
        rec_watch_fsk_set(&d);
        snprintf(feedback,sizeof(feedback),"Defaults restored");
        return;
    }
    if(i<0 || i>4)return;
    rec_fsk_mod_t m; rec_watch_fsk_get(&m);
    static const char *const T[]={"BIT RATE 600-300000","DEVIATION 600-200000 Hz",
        "BANDWIDTH 5-467 kHz","PREAMBLE 8-1024 bits","SYNC WORD, DECIMAL"};
    static const char *const U[]={"baud","Hz","kHz","bits","decimal"};
    const double v[]={m.bitrate,m.deviation_hz,m.bandwidth_khz,m.preamble_bits,
                      (double)m.sync_word};
    fsk_item=i;
    ls_numpad_open(T[i],U[i],v[i],fsk_value);
}
static void fsk_setup_open(void)
{
    rec_fsk_mod_t m; rec_watch_fsk_get(&m);
    char d0[24],d1[24],d2[24],d3[24],d4[32];
    snprintf(d0,sizeof(d0),"%lu baud",(unsigned long)m.bitrate);
    snprintf(d1,sizeof(d1),"%.1f kHz",m.deviation_hz/1000.0);
    snprintf(d2,sizeof(d2),"%u kHz",(unsigned)m.bandwidth_khz);
    snprintf(d3,sizeof(d3),"%u bits",(unsigned)m.preamble_bits);
    /* Hex here because that is how a sync word is written everywhere else,
       and decimal on the keypad because that is all it can type. */
    snprintf(d4,sizeof(d4),"%08lX",(unsigned long)m.sync_word);
    char title[24];
    snprintf(title,sizeof(title),"%s RECEIVE",ls_rsel_name(LS_RSEL_LORA));
    ls_picker_open(title,fsk_done);
    ls_picker_add("Bit rate",d0);
    ls_picker_add("Deviation",d1);
    ls_picker_add("Bandwidth",d2);
    ls_picker_add("Preamble",d3);
    ls_picker_add("Sync word",d4);
    ls_picker_add("Restore defaults",LS_TEXT(t_scr_subghz_4800_25k_2dd42dd4, "4800 / 25k / 2DD42DD4"));
}
static void setup_done(int i)
{
    if(rec_watch_enabled()) {LS_SNPUTS(t_scr_subghz_stop_watch_before_changing, feedback, sizeof(feedback), "Stop WATCH before changing capture setup");return;}
    if(i==5) {
        rec_disarm();rec_set_thresh(0);rec_set_gap_ms(30);rec_set_min_pulse(40);
        rec_set_max_span(8000000);rec_set_min_edges(6);
        LS_SNPUTS(t_scr_subghz_capture_defaults_restored, feedback, sizeof(feedback), "Capture defaults restored");return;
    }
    if(i<0 || i>4)return;
    setup_item=i;
    const char *title[]={"THRESHOLD 0=AUTO,1-255","END GAP 2-2000 ms","MIN PULSE 4-10000 us","MAX SPAN 10-30000 ms","MIN EDGES 6-4096"};
    const char *unit[]={"magnitude","ms","us","ms","edges"};
    double value[]={rec_get_thresh(),rec_get_gap_ms(),rec_get_min_pulse(),rec_get_max_span()/1000.,rec_get_min_edges()};
    ls_numpad_open(title[i],unit[i],value[i],setup_value);
}
/* RECORD keeps a raw copy of one burst: REC's slicer on an SDR, or the
   LR2021's own I/Q. The CC1101 and the SX1262 have neither. */
static bool can_record(void)
{
    return rec_watch_source()==REC_SOURCE_RTL ||
           (rec_watch_source()==REC_SOURCE_SX1262 && ls_lora_chip()==LS_LORA_CHIP_LR20XX);
}
/* LEARN measures a 2-FSK transmitter, which only the LoRa chip demodulates. */
static bool can_learn(void) { return rec_watch_source()==REC_SOURCE_SX1262; }

/* WHY a control is greyed out, in the words of the thing blocking it.

   A disabled button that says nothing when pressed teaches the operator that
   the screen is broken. Every one of these is a real precondition somewhere
   below; saying it out loud costs a line and removes the guessing. NULL means
   the control is live. */
static const char *disabled_reason(char c)
{
    const bool watching=rec_watch_enabled();
    const bool learning=rec_watch_learn_busy();
    /* RECORD holds the receiver armed until a burst lands; anything that
       retunes or takes the radio waits for it, and D is the way out. */
    if(rec_mine && (c=='f' || c=='r' || c=='s' || c=='n' || c=='w' || c=='l'))
        return "Stop RECORD first (D)";
    if(rec_watch_iq_status(NULL,0) && (c=='f' || c=='r' || c=='s' || c=='n' || c=='w' || c=='l'))return "Stop I/Q recording first";
    switch(c) {
    case 'w': return !s.ready?"Still loading capture history from the card":
                     learning?"LEARN has the radio - L stops it":
                     !watching && rec_watch_scan_busy()?"Stop SCAN before WATCH":NULL;
    case 'f': return watching?"Stop WATCH before retuning":
                     rec_watch_scan_busy()?"Stop SCAN before retuning":
                     learning?"Stop LEARN before retuning":NULL;
    /* Stopping a sweep must never be refused. Starting one stops WATCH
       itself, once a band is picked: the sweep and the receiver want the
       same radio, and making somebody stop one by hand to start the other
       is the compartment this screen is trying not to be. */
    case 'n': return rec_watch_scan_busy()?NULL:
                     learning?"Stop LEARN before sweeping":NULL;
    case 's': return watching?"Stop WATCH before changing capture settings":
                     rec_watch_scan_busy()?"Stop SCAN before changing capture settings":
                     learning?"Stop LEARN before changing capture settings":
                     rec_watch_source()==REC_SOURCE_CC1101?
                     "CC1101 capture settings are fixed":
                     NULL;
    case 'r': return watching?"Stop WATCH before changing source":
                     learning?"Stop LEARN before changing source":NULL;
    case 'p': return s.count?NULL:"Nothing captured yet";
    case 'j': return s.count?NULL:"Nothing captured yet";
    case 'e': return !s.count?"Nothing captured yet":
                     s.exporting?"An export is already running":NULL;
    /* Stopping LEARN is never refused; starting it stops WATCH and brings it
       back afterwards, so WATCH is not a reason either. */
    case 'l': {
        EXT_RAM_BSS_ATTR static char lora_only[48];
        if(learning)return NULL;
        snprintf(lora_only,sizeof(lora_only),"LEARN measures on the %s - pick it in RADIO",ls_rsel_name(LS_RSEL_LORA));
        return !can_learn()?lora_only:
               rec_watch_scan_busy()?"Stop SCAN before LEARN":NULL;
    }
    case 'd': case 'v': {
        EXT_RAM_BSS_ATTR static char rtl_only[48];
        if(rec_mine || (c=='v' && on_band()))return NULL;
        LS_SNPRINTF(t_scr_subghz_record_records_with, rtl_only,sizeof(rtl_only),"RECORD needs the %s or %s",ls_rsel_name(LS_RSEL_SDR_RTL),ls_rsel_name(LS_RSEL_SDR_HACKRF));
        return !can_record()?rtl_only:
               rec_watch_scan_busy()?"Stop SCAN before RECORD":
               learning?"Stop LEARN before RECORD":NULL;
    }
    default:  return NULL;
    }
}

/* The menus call straight into it, and it is defined below them. */
static bool s_iq_view;
static void iq_view_stop(void)
{ if (s_iq_view) { ls_wf_source_release(); s_iq_view = false; } }

static void action(char c);

/* A picker returns the index it was added at, and these lists are built
   conditionally - a capture that cannot be replayed has no REPLAY row - so
   the index alone does not say which action was chosen. This remembers what
   each row meant. */
/* Twelve. It was eight, and MORE reached nine - menu_add silently dropped
   the last one, so DETAILS was simply missing from the list. MORE is five
   rows now and the longest list here is TUNE at eight, but the cap stays
   above the longest of them with room to spare: a cap that discards the
   caller's input without saying so is worse than one that is too small. */
static char menu_id[12];
static int  menu_n;
static void menu_add(char id, const char *label, const char *detail)
{
    if (menu_n < (int)sizeof(menu_id) && ls_picker_add(label, detail))
        menu_id[menu_n++] = id;
}

/* How hard to send it.

   It used to be 14 dBm with no say in the matter, which is the wrong default
   in both directions: too much for a receiver on the bench a foot away, and
   not enough for the thing you are actually trying to reach. The four steps
   are the radio's own, and the list says what each one is for rather than
   leaving the operator to know what a dBm is. Replay stops at +10 dBm on
   every radio (subghz_tx_refusal). */
#define TX_STEPS SUBGHZ_TX_STEPS
#define TX_STEP_NAME(i) subghz_tx_step_name(i)
static int tx_step;
static bool tx_resume = true;
static int tx_pend_kind;
static uint32_t tx_pend_id;
static int tx_pend_dbm;
static int64_t tx_pend_until;
static bool tx_paused_watch, tx_waiting;
static bool watch_set(bool on);

static int tx_dbm(uint8_t source)
{
    return subghz_tx_step_dbm(tx_step, source == REC_SOURCE_CC1101);
}
static const rec_watch_event_t *picked(void)
{
    for (int i = 0; i < s.count; i++)
        if (s.event[i].id == pick_id) { selected = i; return &s.event[i]; }
    return selected < s.count ? &s.event[selected] : NULL;
}
static int tx_face_dbm(void)
{
    const rec_watch_event_t *e = picked();
    return tx_dbm(e ? e->source : (uint8_t)rec_watch_source());
}
static bool tx_busy(void)
{
    return tx_pend_kind || tx_waiting || rec_watch_replay_status(NULL, 0);
}
static void tx_power_step(int d, bool wrap)
{
    if (tx_busy()) { snprintf(feedback, sizeof(feedback), "Wait for the send to finish"); return; }
    int n = tx_step + d;
    if (wrap) n = (n % TX_STEPS + TX_STEPS) % TX_STEPS;
    else if (n < 0) n = 0;
    else if (n >= TX_STEPS) n = TX_STEPS - 1;
    tx_step = n;
    ls_rec_player_set_power(n);
    snprintf(feedback, sizeof(feedback), "TX power %s, %+d dBm", TX_STEP_NAME(n), tx_face_dbm());
}
static const char *tx_blocked(void)
{
    if (rec_watch_scan_busy()) return "Stop SCAN before sending";
    if (rec_watch_learn_busy()) return "Stop LEARN before sending";
    if (rec_mine) return "Stop RECORD before sending";
    if (rec_watch_iq_status(NULL, 0)) return "Stop I/Q recording first";
    if (tx_busy()) return "Still sending - wait for it to finish";
    return NULL;
}
static void tx_finish(void)
{
    if (!tx_paused_watch) return;
    tx_paused_watch = false;
    if (tx_resume && !rec_watch_enabled()) watch_set(true);
}
static void tx_pump(void)
{
    if (tx_pend_kind) {
        const bool queued = tx_pend_kind == 1 ? rec_watch_request_replay(tx_pend_id, tx_pend_dbm)
                                              : ls_rec_player_play();
        if (queued) { tx_pend_kind = 0; tx_waiting = true; }
        else if (esp_timer_get_time() > tx_pend_until) {
            tx_pend_kind = 0;
            snprintf(feedback, sizeof(feedback), "Radio busy - press SEND again");
            tx_finish();
        }
        return;
    }
    if (tx_waiting && !rec_watch_replay_status(NULL, 0)) { tx_waiting = false; tx_finish(); }
}
static bool tx_take_radio(void)
{
    const char *no = tx_blocked();
    if (no) { snprintf(feedback, sizeof(feedback), "%s", no); return false; }
    if (rec_watch_enabled()) {
        if (!watch_set(false)) { snprintf(feedback, sizeof(feedback), "Could not pause WATCH to send"); return false; }
        tx_paused_watch = true;
    }
    tx_pend_until = esp_timer_get_time() + 3000000;
    return true;
}
static void tx_capture(const rec_watch_event_t *e)
{
    if (!e) { LS_SNPUTS(t_scr_subghz_nothing_captured_yet, feedback, sizeof(feedback), "Nothing captured yet"); return; }
    if (e->bitrate && !rec_source_can_replay((rec_source_t)e->source)) {
        snprintf(feedback, sizeof(feedback), "%s cannot transmit this capture",
                 ls_rsel_name(rec_source_radio((rec_source_t)e->source)));
        return;
    }
    if (!tx_take_radio()) return;
    tx_pend_kind = 1;
    tx_pend_id = e->id;
    tx_pend_dbm = tx_dbm(e->source);
    LS_SNPRINTF(t_scr_subghz_sending_lu_at_d, feedback,sizeof(feedback),"Sending #%lu at %d dBm...",
             (unsigned long)e->id, tx_pend_dbm);
    tx_pump();
}
static void tx_player(void)
{
    ls_rec_player_set_power(tx_step);
    if (!tx_take_radio()) return;
    tx_pend_kind = 2;
    snprintf(feedback, sizeof(feedback), "Sending at %s power...", TX_STEP_NAME(tx_step));
    tx_pump();
}
static void replay_power_done(int index)
{
    if (index < 0 || index >= TX_STEPS) return;
    rec_watch_snapshot(&s);
    tx_step = index;
    ls_rec_player_set_power(index);
    tx_capture(picked());
}
static void replay_power_open(void)
{
    static const char *const WHY[SUBGHZ_TX_STEPS_MAX] = {
        "Bench test, a few feet",
        "Same room",
        "Normal range",
        LS_TX_UNLOCKED ? "Unlocked" : "Maximum - check your licence",
        "Unlocked",
        "Unlocked",
        "Chip maximum, unlocked"
    };
    static char label[SUBGHZ_TX_STEPS_MAX][16];
    const rec_watch_event_t *e = picked();
    const bool cc = e && e->source==REC_SOURCE_CC1101;
    if (cc) ls_picker_open(LS_TEXT(t_scr_subghz_cc1101_nominal_power, "CC1101 NOMINAL POWER"),replay_power_done);
    else ls_picker_open("SEND AT", replay_power_done);
    for (int i = 0; i < TX_STEPS; i++) {
        snprintf(label[i], sizeof(label[i]), "%d dBm", subghz_tx_step_dbm(i, cc));
        ls_picker_add(label[i], i == tx_step ? (cc ? "in use - send once" : "in use") : cc ? "Send RAW OOK once" : WHY[i]);
    }
}

static void capture_menu_done(int index)
{
    if (index < 0 || index >= menu_n) return;
    rec_watch_snapshot(&s);
    const rec_watch_event_t *e = picked();
    if (!e) return;
    switch (menu_id[index]) {
    case 'R': tx_capture(e); break;
    case 'Q': replay_power_open(); break;
    case 'S':
        if (!rec_watch_request_export(e->id))
            LS_SNPUTS(t_scr_subghz_export_busy_or_capture, feedback, sizeof(feedback), "Export busy or capture unavailable");
        break;
    case 'P':
        if (!rec_watch_request_pin(e->id,!e->pinned))
            snprintf(feedback,sizeof(feedback),"Pin request busy");
        break;
    case 'J': action('j'); break;
    default: break;
    }
}

/* FIND THE SIGNAL FIRST.

   Everything else on this screen assumes you already know the frequency. A
   sweep is the one part of "what is out there" that can be measured rather
   than guessed, so it comes first and the answer is offered as something to
   tune to rather than a number to copy down. */
/* THE BAND, AS IT IS, FILLING THE PANE.

   A percentage on a thin bar tells you the firmware is alive. It does not
   tell you whether anything is out there, which is the only question a scan
   exists to answer. Thirty-two bins drawn as columns do: press the remote
   and a column jumps, and you can see which one before any list is written.

   The column heights are scaled between this pass's floor and the strongest
   thing seen so far, so the display uses its whole height whether the band
   is dead quiet or full. A fixed dBm range would spend most of its height
   on nothing. */
/* How the sweep is drawn. The palettes and the grain names are the
   waterfall's, deliberately - two spectra in one firmware that disagree
   about what yellow means is worse than either choice. ls_wf_level_colour is
   shared; the glyph ramps are short enough to mirror. */
/* Loaded on the first draw and saved on every change. -1 means not yet
   read; settings is not up when these statics are initialised. */
static int scan_look=-1;     /* 0 = dB bands, else palette scan_look-1 */
/* Bars or a text ramp, nothing else. There was a SHADE grain of dither
   blocks; a column of them read as a mottled slab, not a level, and the
   owner said so. It is gone, and a board that had it stored gets BARS. */
static int scan_grain=-1;    /* 0 bars (eighth-block tops), 1 ascii, 2 waterfall */
/* How many dB above the floor the top of the display means. Wide enough
   that noise stays down in the grass and a signal has somewhere to go. */
#define SCAN_WINDOW_DB 45.0f
#define SCAN_LOOKS (1 + LS_WF_PAL__COUNT)
#define SCAN_GRAINS 3
#define SCAN_GRAIN_ASCII 1
#define SCAN_GRAIN_WATERFALL 2
/* Stored as 3: the old SHADE/ASCII/BARS setting used 0, 1 and 2, and a
   board that had BARS stored must not come up as a waterfall. */
#define SCAN_GRAIN_WATERFALL_STORED 3
static const char *const SCAN_LOOK_NAME[SCAN_LOOKS] =
    {"dB BANDS","HEAT","ICE","PHOSPHOR","NEON"};
static const char *const SCAN_GRAIN_NAME[SCAN_GRAINS] =
    {"BARS","ASCII","WATERFALL"};

static void scan_look_load(void)
{
    if(scan_look<0) scan_look=settings_get_subghz_colour();
    /* The stored value once counted SHADE 0, ASCII 1, BARS 2. */
    if(scan_grain<0) {
        const int st=settings_get_subghz_style();
        scan_grain=st==SCAN_GRAIN_ASCII?SCAN_GRAIN_ASCII:st==SCAN_GRAIN_WATERFALL_STORED?SCAN_GRAIN_WATERFALL:0;
    }
    if(scan_look>=SCAN_LOOKS) scan_look=0;
}

/* Colour IS the reading, not decoration: each band is a real number of dB
   above the measured noise floor, and the legend on screen says which. A
   palette chosen to look busy would make a quiet band look like a find. */
static uint8_t scan_hue(float over_floor)
{
    if (over_floor < rec_watch_scan_threshold())
        return over_floor >= 4.0f ? TUI_BLUE : TUI_CYAN;
    if (over_floor >= 40.0f) return TUI_RED;       /* very close, or strong */
    if (over_floor >= 25.0f) return TUI_YELLOW;    /* comfortably readable  */
    if (over_floor >= rec_watch_scan_threshold()) return TUI_GREEN;   /* a detection */
    if (over_floor >= 4.0f)  return TUI_BLUE;      /* something, under the gate */
    return TUI_CYAN;                               /* noise */
}

/* Sixteen levels, which is what the shared palettes are indexed by. */
static int scan_level(float over_floor, float span)
{
    int l = (int)(over_floor / (span < 1.0f ? 1.0f : span) * 15.0f + 0.5f);
    if (l < 0) l = 0;
    if (l > 15) l = 15;
    return l;
}
static uint8_t scan_colour(float over_floor, float span)
{
    if (!scan_look) return scan_hue(over_floor);
    return ls_wf_level_colour(scan_look - 1, scan_level(over_floor, span), false);
}
/* `eighths` is how much of the top cell is filled, 1..8, and only the BARS
   grain can express it - which is the point of having it. */
static char scan_glyph(int level, int eighths)
{
    if (scan_grain == SCAN_GRAIN_ASCII) {
        static const char RAMP[] = " .:-=+*#%@";
        int i = level * 10 / 16;
        if (i < 0) i = 0;
        if (i > 9) i = 9;
        return RAMP[i];
    }
    if (eighths > 0) return LS_TUI_TRACE(eighths > 8 ? 8 : eighths);
    return LS_TUI_TRACE(8);
}

/* The same look, for the other plots that want to read like this one. */
void ls_scan_look_load(void) { scan_look_load(); }
uint8_t ls_scan_colour(float over, float span) { return scan_colour(over, span); }
int ls_scan_level(float over, float span) { return scan_level(over, span); }
char ls_scan_glyph(int level, int eighths) { return scan_glyph(level, eighths); }

static void tx_gauge(tui_surface *sf, tui_rect box)
{
    if (box.w < 30 + (LS_TX_UNLOCKED ? 18 : 0) || box.h < 3) return;
    scan_look_load();
    char t[24];
    snprintf(t, sizeof(t), LS_TX_UNLOCKED ? " UNLOCKED TX %+d dBm " : " TX %+d dBm ", tx_face_dbm());
    const int tw = (int)strlen(t);
    const int x = box.x + box.w - tw - TX_STEPS * 2 - 3;
    const int y = box.y + box.h - 1;
    const bool blink = tx_busy() && (esp_timer_get_time() / 160000) % 2 == 0;
    tui_put_str(sf, box, x, y, t, blink ? TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT)
                                  : LS_TX_UNLOCKED ? TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK)
                                  : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    for (int j = 0; j < TX_STEPS; j++) {
        const float over = 10.0f + 36.0f * j / (TX_STEPS - 1);
        const bool lit = j <= tx_step;
        const char g = lit ? scan_glyph(12, 2 + 2 * j) : LS_TUI_TRACE(1);
        const uint8_t at = lit ? TUI_ATTR(scan_colour(over, SCAN_WINDOW_DB) | TUI_BRIGHT, TUI_BLACK)
                               : LS_ATTR_DIM;
        tui_put_char(sf, box, x + tw + j * 2, y, g, at);
        tui_put_char(sf, box, x + tw + j * 2 + 1, y, g, at);
    }
}

/* SOMETHING HAPPENED, said without sound or vibration.

   Not everyone wants the device buzzing, and a detection that only shows up
   as a number quietly incrementing is a detection nobody notices. This
   flashes for a second and a bit: long enough to catch an eye that was
   somewhere else, short enough not to sit there afterwards pretending it is
   still happening. */
#define HIT_FLASH_US 1400000
static int64_t hit_flash_us;
static int hit_flash_seen;
static uint32_t hit_flash_hz;
static void hit_flash_poll(void)
{
    const int hits = rec_watch_scan_events();
    if (hits > hit_flash_seen) {
        hit_flash_us = esp_timer_get_time();
        hit_flash_hz = rec_watch_scan_last_hit();
    }
    hit_flash_seen = hits;
}
static bool hit_flashing(void)
{
    return hit_flash_us &&
           esp_timer_get_time() - hit_flash_us < HIT_FLASH_US;
}
/* On for 160 ms, off for 160 ms: a blink, not a strobe. */
static bool hit_flash_on(void)
{
    if (!hit_flashing()) return false;
    return ((esp_timer_get_time() - hit_flash_us) / 160000) % 2 == 0;
}

/* The detections strongest first, as the table lists them. A finished sweep
   is already sorted; a running one is in the order things were found, so
   ENTER has to sort the same way or it opens a different row from the one
   that is lit. */
static int hits_sorted(rec_scan_bin_t *hit)
{
    int n = rec_watch_scan_result(hit, REC_SCAN_BINS);
    if (n < 0) n = 0;
    for (int i = 1; i < n; i++) {
        const rec_scan_bin_t k = hit[i];
        int j = i - 1;
        while (j >= 0 && hit[j].dbm < k.dbm) { hit[j + 1] = hit[j]; j--; }
        hit[j + 1] = k;
    }
    return n;
}

/* WHAT IT ACTUALLY FOUND, as a table rather than a picker.

   The old answer was a list of frequencies, which cannot distinguish a
   transmitter that spoke once from one that is still going. Count and
   age are what make a detection worth acting on. */
static void hits_table(tui_surface *sf, tui_rect r)
{
    static EXT_RAM_BSS_ATTR rec_scan_bin_t hit[REC_SCAN_BINS];
    const int n = hits_sorted(hit);
    det_row_n = 0;
    const float floor_dbm = rec_watch_scan_busy() ? rec_watch_scan_live_floor()
                                                  : rec_watch_scan_floor();
    char line[110];

    LS_SNPRINTF(t_scr_subghz_detections_d_bursts_on, line, sizeof(line), "DETECTIONS  %d bursts on %d freq, over %.0f dB",
             rec_watch_scan_events(), n, (double)rec_watch_scan_threshold());
    tui_put_str(sf, r, r.x, r.y, line,
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    if (!n) {
        tui_put_str(sf, r, r.x, r.y + 2,
                    "Nothing has crossed the threshold yet.", LS_ATTR_DIM);
        tui_put_str(sf, r, r.x, r.y + 3,
                    "Leave SCAN running and transmit.", LS_ATTR_DIM);
        return;
    }
    LS_SNPRINTF(t_scr_subghz_11s_7s_5s_5s, line, sizeof(line), "%-11s %7s %5s %5s %8s",
             "MHz", "dBm", "over", "seen", "last");
    tui_put_str(sf, r, r.x, r.y + 1, line, LS_ATTR_DIM);

    const int64_t now = esp_timer_get_time();
    const int rows = r.h - 3;
    det_total=n;
    if(det_sel>=n)det_sel=n-1;
    if(det_sel<0)det_sel=0;
    if(!det_free)det_first=rows>0?det_sel/rows*rows:0;
    if(rows>0 && n>rows &&
       ls_drag_scroll(&det_drag,tui_rect_make(r.x,r.y+2,r.w,rows),1,&det_first,
                      drag_span(&det_drag,det_first,n,rows),rows))det_free=true;
    if(det_first>=n)det_first=n-1;
    if(det_first<0)det_first=0;
    det_pager=tui_rect_make(r.x,r.y+r.h-1,r.w,1);
    det_rows = tui_rect_make(r.x, r.y + 2, r.w, rows < n ? rows : n);
    det_row_n = 0;
    for (int i = det_first; i < n && i < det_first+rows; i++) {
        if (det_row_n < DET_ROWS_MAX) det_row_hz[det_row_n++] = hit[i].hz;
        const float over = hit[i].dbm - floor_dbm;
        char age[12];
        if (!hit[i].last_us) snprintf(age, sizeof(age), "--");
        else {
            const uint32_t s_ago = (uint32_t)((now - hit[i].last_us) / 1000000);
            if (s_ago < 60) snprintf(age, sizeof(age), "%lus", (unsigned long)s_ago);
            else snprintf(age, sizeof(age), "%lum", (unsigned long)(s_ago / 60));
        }
        LS_SNPRINTF(t_scr_subghz_11_4f_7_0f, line, sizeof(line), "%-11.4f %7.0f %5.0f %5lu %8s",
                 hit[i].hz / 1e6, (double)hit[i].dbm, (double)over,
                 (unsigned long)hit[i].seen, age);
        /* The strongest is what a glance should land on. */
        tui_put_str(sf, r, r.x, r.y + 2 + i-det_first, line,
                    i == det_sel ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
                           : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    }
    if(n>rows) {
        tui_put_str(sf,r,r.x,r.y+r.h-1,"< PREV",LS_ATTR_DIM);
        tui_put_str(sf,r,r.x+r.w-6,r.y+r.h-1,"NEXT >",LS_ATTR_DIM);
        tui_put_str(sf,r,r.x+8,r.y+r.h-1,"UP/DOWN, ENTER",LS_ATTR_DIM);
    } else tui_put_str(sf,r,r.x,r.y+r.h-1,"UP/DOWN select, ENTER or tap to act",LS_ATTR_DIM);
}

/* Columns left of the track that still grab it. */
#define GATE_REACH 5

/* The threshold as a height on the plot, from a row inside gate_hit. */
static float gate_db_at_row(int y)
{
    if (y < gate_hit.y) y = gate_hit.y;
    if (y > gate_hit.y + gate_hit.h - 1) y = gate_hit.y + gate_hit.h - 1;
    const int from_bottom = gate_hit.y + gate_hit.h - 1 - y;
    /* The inverse of the drawing: rows are dB over floor + noise. Whole dB:
       a row is a few dB, and "8.25" in the keypad reads as a mistake. */
    return roundf(gate_hit_noise + gate_hit_span * (float)from_bottom / (float)gate_hit.h);
}
/* The row the line was last drawn on, absolute, and where its number sits. */
static int gate_line_y = -1;
static tui_rect gate_label_hit;

/* A finger that goes down on the track sets the gate where it lands and
   drags it, frame by frame, until it lifts - wherever it wanders.

   The track is the physical control, a finger wide down the right edge:
   tap the height you want. For a while only the line itself could be
   grabbed (the gate was once found at 39 dB after the device was carried
   with a thumb on that edge), but a target one row tall on a hairline rail
   could not be hit at all, which is worse; the value now shows while the
   finger is down, and a tap anywhere on the chart never moves it.

   The line moves in memory while the finger is down and is saved once when
   it lifts: a save per frame would be a flash write every 40 ms. */
static bool gate_dragging;
static void gate_follow_drag(void)
{
    int sc, sr, c, r;
    const bool held = gate_hit.h > 0 && ls_tui_touch_held(&sc, &sr, &c, &r);
    if (!held) {
        if (gate_dragging) {
            rec_watch_scan_set_threshold(rec_watch_scan_threshold());
            LS_SNPRINTF(t_scr_subghz_detect_above_0f_db, feedback, sizeof(feedback), "Detect above %.0f dB over the floor",
                     (double)rec_watch_scan_threshold());
        }
        gate_dragging = false;
        return;
    }
    /* A press that starts anywhere on the track takes it: tap the height
       you want, or drag. One that starts on the chart never does. */
    if (!gate_dragging) {
        if (!tui_rect_contains(gate_hit, sc, sr)) return;
        gate_dragging = true;
    }
    rec_watch_scan_preview_threshold(gate_db_at_row(r));
    LS_SNPRINTF(t_scr_subghz_detect_above_0f_db, feedback, sizeof(feedback), "Detect above %.0f dB over the floor",
             (double)rec_watch_scan_threshold());
}

/* What each column of the last drawn chart was, so a tap on a column offers
   that frequency - LISTEN HERE - instead of moving anything. */
static tui_rect chart_hit;
static int chart_n;
static uint32_t chart_hz[REC_SCAN_BINS];
static float chart_dbm[REC_SCAN_BINS];
static uint32_t det_selected_hz;
static int chart_column(int x)
{
    if (!chart_n || chart_hit.w <= 0) return -1;
    int i = (int)((long)(x - chart_hit.x) * chart_n / chart_hit.w);
    if (i < 0) i = 0;
    if (i >= chart_n) i = chart_n - 1;
    return i;
}

/* The detection picked in the list, marked on the chart so the two read as
   one view: 0 for none. */
static uint32_t band_mark_hz;

/* THE WATERFALL: one row a pass, newest at the top, every cell the bin's dB
   over the floor in the sweep's own colours, bright where it clears the
   gate. The bars show this moment; the waterfall shows the last minute,
   which is where a burst that landed between two looks still is. The
   sweep has no pass counter to hand, so a pass is taken as complete when
   its progress wraps - or, on a receiver whose progress does not move,
   every WF_PASS_US. The ring lives in PSRAM: 48 rows of 64 bins. */
#define WF_ROWS 48
#define WF_PASS_US 400000
static EXT_RAM_BSS_ATTR uint8_t wf_ring[WF_ROWS][REC_SCAN_BINS];   /* dB over floor, 0..255 */
static int wf_head, wf_count, wf_progress = -1;
static int wf_total, wf_pix_total = -1;   /* passes into the ring, ever; and into the picture */
static int64_t wf_last_us;
static uint32_t wf_lo_hz;
static int wf_n;

static void wf_push(const rec_scan_bin_t *bins, int n, float floor_dbm)
{
    const int64_t now = esp_timer_get_time();
    const int progress = rec_watch_scan_progress();
    if (n != wf_n || (n && bins[0].hz != wf_lo_hz)) {        /* another band: start again */
        wf_head = wf_count = 0; wf_n = n; wf_lo_hz = n ? bins[0].hz : 0; wf_last_us = 0;
        wf_pix_total = -1;
    }
    const bool wrapped = wf_progress >= 0 && progress < wf_progress;
    const bool stale = wf_progress == progress && now - wf_last_us >= WF_PASS_US;
    wf_progress = progress;
    if (!wrapped && !stale && wf_count) return;
    if (!wf_count && now - wf_last_us < WF_PASS_US) return;
    uint8_t *row = wf_ring[wf_head];
    for (int i = 0; i < REC_SCAN_BINS; i++) {
        float over = i < n ? bins[i].dbm - floor_dbm : 0.0f;
        row[i] = (uint8_t)(over < 0 ? 0 : over > 255 ? 255 : over + 0.5f);
    }
    wf_head = (wf_head + 1) % WF_ROWS;
    if (wf_count < WF_ROWS) wf_count++;
    wf_total++;
    wf_last_us = now;
}

/* Drawn in panel pixels by the renderer FALLS and MUSIC use, not in cells:
   a cell a pass and a cell a bin was a blocky grid with hard steps. Each
   pass is a band of pixel lines, neighbouring bins blend, and the colours
   are FALLS's own map for the sweep's palette - dimmed below the gate, full
   strength from the gate up, so what crossed it stands out on the same
   scale the track sets. */
static ls_pwf_t *wf_pix;              /* PSRAM, made on the first waterfall */
static int wf_pix_n, wf_pix_mark = -2;

static void waterfall_release(void)
{
    if (wf_pix) ls_pwf_release(wf_pix);
}
/* Leaving SUB-GHZ gives the picture's PSRAM back; the ring of passes stays
   and fills a new picture when BAND is drawn again. */
static void waterfall_free(void)
{
    if (!wf_pix) return;
    ls_pwf_free(wf_pix);
#ifdef ESP_PLATFORM
    heap_caps_free(wf_pix);
#else
    free(wf_pix);
#endif
    wf_pix = NULL;
    wf_pix_total = -1;
    wf_pix_mark = -2;
}

static void waterfall_colours(float span, float noise_db, float gate_db)
{
    ls_pwf_stop_t pal[8], out[16];
    const int np = ls_wf_smooth_stops(scan_look ? scan_look - 1 : 0, pal);
    /* Codes are half-dB over the floor; the ground is the noise top, the
       top stop the top of the window, as the columns have them. */
    const float lo = noise_db * 2.0f, hi = span * 2.0f > lo + 8.0f ? span * 2.0f : lo + 8.0f;
    int at_g = (int)((gate_db * 2.0f - lo) * 255.0f / (hi - lo) + 0.5f);
    if (at_g < 1) at_g = 1;
    if (at_g > 255) at_g = 255;
    int n = 0;
    for (int i = 0; i < np && n < 14; i++) {
        if (pal[i].at >= at_g) {
            if (!n || out[n - 1].at < at_g) {
                /* The step at the gate: the colour there, dim then full. */
                const ls_pwf_stop_t a = i ? pal[i - 1] : pal[i], b = pal[i];
                const int d = b.at - a.at;
                const int t = d > 0 ? (at_g - a.at) * 256 / d : 256;
                const ls_pwf_stop_t c = {(uint8_t)at_g, (uint8_t)(a.r + (b.r - a.r) * t / 256),
                                         (uint8_t)(a.g + (b.g - a.g) * t / 256),
                                         (uint8_t)(a.b + (b.b - a.b) * t / 256)};
                out[n] = c;
                out[n].at = (uint8_t)(at_g - 1);
                out[n].r = (uint8_t)(c.r * 2 / 5); out[n].g = (uint8_t)(c.g * 2 / 5); out[n].b = (uint8_t)(c.b * 2 / 5);
                n++;
                out[n++] = c;
            }
            if (pal[i].at > at_g) out[n++] = pal[i];
        } else {
            out[n] = pal[i];
            if (i) { out[n].r = (uint8_t)(pal[i].r * 2 / 5); out[n].g = (uint8_t)(pal[i].g * 2 / 5);
                     out[n].b = (uint8_t)(pal[i].b * 2 / 5); }
            n++;
        }
    }
    ls_pwf_colours(wf_pix, out, n, lo, hi);
}

static void waterfall(tui_surface *sf, tui_rect cells, int n, float span, float noise_db,
                      float gate_db, int mark_bin)
{
    if (cells.w < 4 || cells.h < 2 || n < 2) return;
    if (!ls_tui_has_images()) {
        /* A surface without pictures (the text feed): the fall in cells. */
        for (int y = 0; y < cells.h && y < wf_count; y++) {
            const uint8_t *row = wf_ring[(wf_head - 1 - y + WF_ROWS) % WF_ROWS];
            for (int x = 0; x < cells.w; x++) {
                const int i = (int)((long)x * n / cells.w);
                const float over = row[i];
                if (over < noise_db) continue;
                tui_put_char(sf, cells, cells.x + x, cells.y + y, LS_TUI_BLOCK_FULL,
                             TUI_ATTR(scan_colour(over, span) | (over >= gate_db ? TUI_BRIGHT : 0), TUI_BLACK));
            }
        }
        return;
    }
    int cw = 0, ch = 0;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    if (cw <= 0 || ch <= 0) cw = 10, ch = 17;
    if (!wf_pix) {
#ifdef ESP_PLATFORM
        wf_pix = heap_caps_calloc(1, sizeof(*wf_pix), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        wf_pix = calloc(1, sizeof(*wf_pix));
#endif
        if (!wf_pix) return;
    }
    const int w = cells.w * cw, h = cells.h * ch;
    const bool fresh = !wf_pix->img || wf_pix->w != w || wf_pix->h != h || wf_pix->bins != n;
    if (!ls_pwf_size(wf_pix, LS_PWF_DOWN, w, h, n)) {
        ls_safe_line(sf, cells, cells.y + cells.h / 2, "Not enough PSRAM for the waterfall", LS_ATTR_DIM);
        return;
    }
    if (fresh || wf_pix_n != n) {
        ls_pwf_axis_lerp(wf_pix, 0, n - 1);
        wf_pix_total = -1;
        wf_pix_n = n;
    }
    /* Whole dB on the window and the noise, so a wobbling peak does not
       recolour the history every frame. */
    waterfall_colours(roundf(span / 3.0f) * 3.0f, roundf(noise_db), gate_db);
    /* Every pass the picture has not had, oldest first, each as a band of
       lines: WF_ROWS passes fill it top to bottom. */
    const int per = h / WF_ROWS > 0 ? h / WF_ROWS : 1;
    if (wf_pix_total < 0 || wf_total - wf_pix_total > wf_count) {
        ls_pwf_clear(wf_pix);
        wf_pix_total = wf_total - wf_count;
    }
    for (; wf_pix_total < wf_total; wf_pix_total++) {
        const int back = wf_total - 1 - wf_pix_total;          /* 0 = newest */
        const uint8_t *row = wf_ring[(wf_head - 1 - back + WF_ROWS) % WF_ROWS];
        for (int k = 0; k < per; k++) {
            uint8_t *line = ls_pwf_push(wf_pix);
            if (!line) break;
            for (int i = 0; i < n; i++) {
                const int c = row[i] * 2;
                line[i] = (uint8_t)(c > 255 ? 255 : c);
            }
        }
    }
    /* The detection picked in the list, as a dotted line down its column. */
    const int mark_px = mark_bin >= 0 ? (int)(((long)mark_bin * 2 + 1) * w / (2L * n)) : -1;
    if (mark_px != wf_pix_mark) {
        ls_pwf_mark(wf_pix, mark_px, 0x07FF);
        wf_pix_mark = mark_px;
    }
    ls_pwf_compose(wf_pix, esp_timer_get_time(), 50000);
    ls_pwf_show(wf_pix, sf, cells);
}

static void spectrum(tui_surface *sf, tui_rect r)
{
    scan_look_load();
    gate_follow_drag();
    static EXT_RAM_BSS_ATTR rec_scan_bin_t bins[REC_SCAN_BINS];
    const int n = rec_watch_scan_live(bins, REC_SCAN_BINS);
    chart_n = 0;
    chart_hit = tui_rect_make(0, 0, 0, 0);
    gate_label_hit = chart_hit;
    gate_line_y = -1;
    if (r.h < 6 || r.w < 20) return;

    const float floor_dbm = rec_watch_scan_live_floor();
    int peak_i = 0;
    for (int i = 0; i < n; i++)
        if (bins[i].dbm > bins[peak_i].dbm) peak_i = i;

    /* A FIXED window above the floor, not a fit to the tallest bin.

       Scaling to the observed peak was why this read as noise: on a quiet
       band the loudest bin IS noise, so a five dB thermal wobble got
       stretched to full height and every column looked like a find. Against
       a fixed 45 dB window, noise occupies the bottom tenth and a real
       signal towers over it - which is the whole point of looking. The
       window only grows, never shrinks, so something genuinely stronger
       still fits. */
    /* And always some room above the tallest column and above the gate,
       so the track can be tapped higher than either: a window that ended
       at the peak left nowhere on it to put a cut-off above a strong
       local transmitter. */
    float top = floor_dbm + SCAN_WINDOW_DB;
    if (bins[peak_i].dbm + 6.0f > top) top = bins[peak_i].dbm + 6.0f;
    if (floor_dbm + rec_watch_scan_threshold() + 6.0f > top)
        top = floor_dbm + rec_watch_scan_threshold() + 6.0f;
    /* Two spreads of noise, at most 10 dB, drawn as the baseline. */
    float noise_db = 2.0f * rec_watch_scan_live_spread();
    if (!(noise_db > 0.0f)) noise_db = 0.0f;
    if (noise_db > 10.0f) noise_db = 10.0f;

    /* Header: what it is doing, and the loudest thing it can hear. */
    char line[96];
    ls_safe_line(sf,r,r.y,"",LS_ATTR_DIM);
    if (hit_flashing()) {
        /* The whole header row, so it cannot be mistaken for the usual
           status text, and inverted on the blink. */
        if (hit_flash_hz) snprintf(line, sizeof(line), " HIT  %.4f MHz ", hit_flash_hz / 1e6);
        else snprintf(line, sizeof(line), " HIT ");
        tui_put_str(sf, r, r.x, r.y, line,
                    hit_flash_on() ? TUI_ATTR(TUI_BLACK, TUI_GREEN | TUI_BRIGHT)
                                   : TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    } else {
        snprintf(line, sizeof(line), "%s", rec_watch_scan_stage());
        tui_put_str(sf, r, r.x, r.y, line[0] ? line : "Sweeping",
                    TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    }
    if (scan_look && r.h >= 8) {
        snprintf(line, sizeof(line), "%s / %s", SCAN_LOOK_NAME[scan_look],
                 SCAN_GRAIN_NAME[scan_grain]);
        if ((int)strlen(line) + 2 < r.w)
            tui_put_str(sf, r, r.x, r.y + r.h - 2, line, LS_ATTR_DIM);
    }
    if (n) {
        snprintf(line, sizeof(line), "%.4f  %.0f dBm", bins[peak_i].hz / 1e6,
                 (double)bins[peak_i].dbm);
        if ((int)strlen(line) + 2 < r.w)
            tui_put_str(sf, r, r.x + r.w - (int)strlen(line), r.y, line,
                        TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }

    const int top_row = r.y + 1;
    const int rows = r.h - 3;
    /* The detections' paging is the list's: the chart draws beside it now,
       and must not page it by the number of live bins. */
    if (rows < 3 || !n) return;

    /* Columns spanning the WHOLE width, edge to edge.

       Dividing the width by the bin count and using that for every column
       left the remainder blank - in portrait that is a quarter of the pane,
       which is what "not filling the width" was. Each column now runs from
       i*w/n to (i+1)*w/n, so they tile the pane exactly and the last one
       ends on the last usable cell rather than short of it. */
    const float span = top - floor_dbm < 1.0f ? 1.0f : top - floor_dbm;
    const int eighth_rows = rows * 8;

    /* The detection threshold, across the display and UNDER the bars.

       Without it, "did anything actually go off" needs arithmetic; with it,
       it needs a look. Drawn first on purpose: a column that reaches the
       threshold paints over it, so the line is visible exactly where nothing
       got there. */
    const float gate_db = rec_watch_scan_threshold();
    /* The gate's track has its own lane at the right edge, and the columns
       stop short of it: drawn over it, the last bins painted out the
       control below the line. */
    const bool lane = r.w >= 20 && rows >= 4;
    const tui_rect pr = lane ? tui_rect_make(r.x, r.y, r.w - 4, r.h) : r;
    /* On the columns' own scale - they rise from floor + noise_db - so a
       column that reaches the line is one that crosses the gate, and the
       number on the marker is still dB over the floor. */
    int gate_row = rows - 1 - (int)((gate_db - noise_db) / span * rows + 0.5f);
    if (gate_row > rows - 1) gate_row = rows - 1;
    {
        if (gate_row > 0 && gate_row < rows)
            for (int x = 0; x < pr.w; x += 2)
                tui_put_char(sf, r, r.x + x, top_row + gate_row, '-',
                             TUI_ATTR(TUI_RED, TUI_BLACK));
    }

    /* DRAG IT, down the whole side of the plot.

       Twelve dB over the floor is a guess about a room, and the operator can
       see where the line ought to sit - just above the grass, just under the
       thing they are trying to catch. A pair of three-cell buttons made that
       a fiddly repeated poke; a full-height track is one tap at the height
       you want, read against the same scale as the bars next to it.

       Light on purpose: it is a backdrop with a marker on it, and it must
       not compete with the measurement it sits beside. */
    if (lane) {
        const int gw = 3;
        const int gx = r.x + r.w - gw;
        /* The target is wider than the track: the track sits against the
           panel's edge, where a fingertip lands short of it. */
        gate_hit = tui_rect_make(gx - GATE_REACH, top_row, gw + GATE_REACH, rows);
        gate_hit_span = span;
        gate_hit_noise = noise_db;
        /* A solid track the full height, a finger wide - it is the control,
           and a hairline was too thin to hit - with the marker bright on it.
           Solid, not dithered: no shade slabs on this chart. */
        for (int y = 0; y < rows; y++)
            for (int k = 0; k < gw; k++)
                tui_put_char(sf, r, gx + k, top_row + y, LS_TUI_BLOCK_FULL,
                             TUI_ATTR(y == gate_row ? (TUI_RED | TUI_BRIGHT) : TUI_RED, TUI_BLACK));
        /* The number rides with the marker, one row clear of the track ends
           so it is never half off the pane. */
        char gt[8];
        snprintf(gt, sizeof(gt), "%2.0f", (double)gate_db);
        int gy = gate_row;
        if (gy < 0) gy = 0;
        if (gy > rows - 1) gy = rows - 1;
        tui_put_str(sf, r, gx, top_row + gy, gt,
                    TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT));
        gate_line_y = top_row + gy;
        gate_label_hit = tui_rect_make(gx, top_row + gy, gw, 1);
    } else {
        gate_hit = tui_rect_make(0, 0, 0, 0);
    }
    /* Every column remembers what it is, for a tap that asks. The track's
       columns are the gate's, not a frequency's. */
    chart_n = n;
    chart_hit = tui_rect_make(pr.x, top_row, pr.w, rows);
    for (int i = 0; i < n && i < REC_SCAN_BINS; i++) {
        chart_hz[i] = bins[i].hz;
        chart_dbm[i] = bins[i].dbm;
    }
    wf_push(bins, n, floor_dbm);
    const bool falls = scan_grain == SCAN_GRAIN_WATERFALL;
    if (falls) {
        /* Below the header row, which keeps the peak's reading in text. */
        int mark_bin = -1;
        if (band_mark_hz && n > 1 && band_mark_hz >= bins[0].hz && band_mark_hz <= bins[n - 1].hz) {
            mark_bin = 0;
            for (int i = 1; i < n; i++)
                if (labs((long)bins[i].hz - (long)band_mark_hz) <
                    labs((long)bins[mark_bin].hz - (long)band_mark_hz)) mark_bin = i;
        }
        waterfall(sf, tui_rect_make(pr.x, top_row + 1, pr.w, rows - 1), n, span, noise_db, gate_db, mark_bin);
    } else
        waterfall_release();

    for (int i = 0; i < n && !falls; i++) {
        const int xa = pr.x + (int)((long)i * pr.w / n);
        const int xb = pr.x + (int)((long)(i + 1) * pr.w / n);
        if (xb <= xa) continue;

        const float over = bins[i].dbm - floor_dbm;
        /* The height starts above the noise this receiver shows, not at the
           floor: a receiver whose quiet readings scatter (the LR2021 at 433
           MHz, +/-4 dB) stood every column 5-15 dB up and read as a band
           full of signals beside a deaf one that sat on its floor. Colour,
           the gate and detection still use the reading itself. */
        float v = (over - noise_db) / span;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        /* In eighths of a cell, so the BARS grain can draw a partial top. */
        const int he = (int)(v * eighth_rows + 0.5f);
        const int h = (he + 7) / 8;

        float hv = (bins[i].hold - floor_dbm - noise_db) / span;
        if (hv < 0) hv = 0;
        if (hv > 1) hv = 1;
        const int hh = (int)(hv * rows + 0.5f);

        const uint8_t hue = scan_colour(over, span);
        const int level = scan_level(over, span);
        const bool strong = over >= gate_db;

        for (int y = 0; y < rows; y++) {
            const int from_bottom = rows - 1 - y;
            char g;
            uint8_t attr;
            if (from_bottom < h - 1) {
                g = scan_glyph(level, 8);
                attr = TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK);
            } else if (from_bottom < h) {
                const int part = he - from_bottom * 8;
                g = scan_glyph(level, part < 1 ? 1 : part);
                attr = TUI_ATTR(hue | (strong ? TUI_BRIGHT : 0), TUI_BLACK);
            }
            /* The decaying peak, as a cap above the live column. This is what
               catches a burst that landed between two looks at this bin,
               which at 60 ms a press is most of them. */
            else if (from_bottom == hh - 1 && hh > h) {
                const float ho = bins[i].hold - floor_dbm;
                g = scan_glyph(scan_level(ho, span), 1);
                attr = TUI_ATTR(scan_colour(ho, span) | TUI_BRIGHT, TUI_BLACK);
            } else if (from_bottom == 0) {
                /* The floor: one thin line, the same in every column. The
                   old quarter-shade row was a mottled strip across the
                   bottom of every sweep. */
                g = LS_TUI_TRACE(1);
                attr = TUI_ATTR(TUI_CYAN, TUI_BLACK);
            } else continue;
            for (int x = xa; x < xb; x++)
                tui_put_char(sf, r, x, top_row + y, g, attr);
        }
    }

    /* WHERE the peak is, as a line down the display.

       The frequency was only in the header, which means reading it and then
       looking back down to work out which column it belonged to. A marker on
       the peak column answers "which one" directly, and it is drawn in the
       gap ABOVE the bar so it never covers the reading it points at. */
    {
        const int px = pr.x + (int)((long)peak_i * pr.w / n) + (pr.w / n) / 2;
        float pv = (bins[peak_i].dbm - floor_dbm) / span;
        if (pv < 0) pv = 0;
        if (pv > 1) pv = 1;
        const int ph = (int)(pv * rows + 0.5f);
        /* On a hit the marker blinks with the header, so the eye is led from
           one to the other rather than having to find the column itself. */
        const bool blink = hit_flash_on();
        for (int y = 0; y < rows - ph && !falls; y++)
            tui_put_char(sf, r, px, top_row + y, blink ? '#' : '|',
                         blink ? TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
                               : TUI_ATTR(TUI_YELLOW, TUI_BLACK));
        /* The reading goes BESIDE the marker, at the top of the column it
           belongs to - not on the axis row, where it was just another
           number among the band edges and you still had to work out which
           column it went with. */
        char pk[24];
        snprintf(pk, sizeof(pk), "%.4f %.0f", bins[peak_i].hz / 1e6,
                 (double)bins[peak_i].dbm);
        const int pw = (int)strlen(pk);
        /* To the right of the line, or to its left when that would run off
           the pane. */
        int tx = px + 2;
        if (tx + pw > r.x + r.w) tx = px - pw - 1;
        if (tx < r.x) tx = r.x;
        if (tx + pw <= r.x + r.w)
            tui_put_str(sf, r, tx, top_row, pk,
                        TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }

    /* The detection picked in the list, as a dotted line down its column:
       the list says how often, the chart says where. */
    if (!falls && band_mark_hz && n > 1 && band_mark_hz >= bins[0].hz && band_mark_hz <= bins[n - 1].hz) {
        int best = 0;
        for (int i = 1; i < n; i++)
            if (labs((long)bins[i].hz - (long)band_mark_hz) <
                labs((long)bins[best].hz - (long)band_mark_hz)) best = i;
        const int mx = pr.x + (int)((long)best * pr.w / n) + (pr.w / n) / 2;
        float mv = (bins[best].dbm - floor_dbm) / span;
        if (mv < 0) mv = 0;
        if (mv > 1) mv = 1;
        const int mh = (int)(mv * rows + 0.5f);
        for (int y = 1; y < rows - mh; y++)
            tui_put_char(sf, r, mx, top_row + y, ':',
                         TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    }

    /* While the line is held, its value beside it, big enough to read past
       the thumb on it - the feedback line is at the other end of the pane. */
    if (gate_dragging && gate_line_y >= 0 && gate_hit.w > 0) {
        char call[24];
        snprintf(call, sizeof(call), " DETECT %.0f dB ", (double)gate_db);
        const int cw = (int)strlen(call);
        const int cx = gate_hit.x - cw;
        if (cx >= r.x)
            tui_put_str(sf, r, cx, gate_line_y, call,
                        TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT));
    }

    /* The band edges, on the same row, kept clear of the peak label. */
    snprintf(line, sizeof(line), "%.3f", bins[0].hz / 1e6);
    tui_put_str(sf, r, r.x, r.y + r.h - 1, line, LS_ATTR_DIM);
    snprintf(line, sizeof(line), "%.3f", bins[n - 1].hz / 1e6);
    if ((int)strlen(line) + 2 < r.w)
        tui_put_str(sf, r, r.x + r.w - (int)strlen(line), r.y + r.h - 1, line,
                    LS_ATTR_DIM);
    /* Two calls rather than one with a chosen format: the branches do not
       take the same arguments, and passing both to whichever won handed the
       int to %f.  The ABI happened to hide it - a double vararg lands in an
       aligned register pair on this part, which is where the callee looks
       anyway - but it printed the wrong number on the host, and a format
       string picked at runtime is not something the compiler can keep
       checking for us. */
    const int hits = rec_watch_scan_hits();
    if (hits) LS_SNPRINTF(t_scr_subghz_d_found_floor_0f, line, sizeof(line), "%d found  floor %.0f dBm",
                       hits, (double)floor_dbm);
    else      LS_SNPRINTF(t_scr_subghz_nothing_yet_floor_0f, line, sizeof(line), "nothing yet  floor %.0f dBm",
                       (double)floor_dbm);
    if ((int)strlen(line) + 2 < r.w)
        tui_put_str(sf, r, r.x + r.w - 1 - (int)strlen(line), r.y + r.h - 2,
                    line, hits ? TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
                               : LS_ATTR_DIM);

    /* What the colours mean, in dB over that floor. Only for the banded
       look: a continuous palette is a ramp, not five named steps, and
       labelling it with five would be a lie about what it shows. */
    if (!scan_look && r.h >= 8 && r.w >= 44) {
        const struct { const char *t; float d; } KEY[] = {
            {"noise", 0}, {"+4", 4}, {"gate", rec_watch_scan_threshold()},
            {"+25", 25}, {"+40", 40}
        };
        int x = r.x;
        for (unsigned i = 0; i < sizeof(KEY)/sizeof(KEY[0]); i++) {
            const int w = (int)strlen(KEY[i].t) + 2;
            if (x + w >= r.x + r.w) break;
            tui_put_char(sf, r, x, r.y + r.h - 3, LS_TUI_BLOCK_FULL,
                         TUI_ATTR(scan_hue(KEY[i].d) | TUI_BRIGHT, TUI_BLACK));
            tui_put_str(sf, r, x + 1, r.y + r.h - 3, KEY[i].t, LS_ATTR_DIM);
            x += w;
        }
    }
}

/* Defined with the pages. */
static void go_read(void);
static bool band_catch;
/* Set when this screen asked the sweep to stop. A sweep that ends without
   being asked, with ON DETECTION LISTEN set and the receiver tuned to the
   last hit, is the runtime's hand-off, and READ follows it. */
static bool scan_stop_asked;
static void sweep_stop(void) { scan_stop_asked=true; rec_watch_scan_stop(); }

/* LISTEN HERE: from "something is there" to "listen to it", in one press.
 *
 * A Flipper's analyser hands its frequency to Read; this hands it to READ:
 * tune READ's receiver to it, start WATCH, and go there - with the dial
 * spinning to the new figures, so the change is seen, not inferred. Whatever
 * held the radio (a sweep, CATCH, WATCH on another frequency) is let go
 * first, because asking the operator to stop it by hand is the step this
 * replaces. A sweep lets go asynchronously; listen_pending carries the
 * frequency across until it has. */
static const char *listen_from = "BAND";
static void listen_here(uint32_t hz)
{
    if (!hz) return;
    if (band_catch || ls_catch_running()) { ls_catch_stop(); band_catch = false; }
    if (rec_watch_learn_busy()) { snprintf(feedback, sizeof(feedback), "Stop LEARN first"); return; }
    if (rec_mine) { snprintf(feedback, sizeof(feedback), "Stop RECORD first"); return; }
    sweep_hits = false;
    if (rec_watch_scan_busy()) {
        /* The runtime starts WATCH itself when READ's radio is the one that
           swept; listen_pending starts it for any other. */
        if (!rec_watch_scan_catch(hz)) {
            LS_SNPUTS(t_scr_subghz_capture_frequency_refused_by, feedback, sizeof(feedback), "Capture frequency refused by selected receiver");
            return;
        }
        source_frequency[rec_watch_source()] = hz;
        listen_pending = hz;
        scan_stop_asked = true;
    } else {
        if (rec_watch_enabled() && rec_get_freq() != hz && !watch_set(false)) {
            LS_SNPUTS(t_scr_subghz_stop_watch_before_retuning, feedback, sizeof(feedback), "Stop WATCH before retuning");
            return;
        }
        if (rec_get_freq() != hz && !rec_set_freq(hz)) {
            LS_SNPUTS(t_scr_subghz_frequency_refused_by_selected, feedback, sizeof(feedback), "Frequency refused by selected receiver");
            return;
        }
        source_frequency[rec_watch_source()] = hz;
        if (!rec_watch_enabled() && !watch_set(true)) {
            /* Still READ: the frequency is set, and READ is where WATCH is
               retried from. */
            const char *why = rec_watch_error();
            go_read();
            ls_fresh_bump(&handoff);
            snprintf(feedback, sizeof(feedback), "Tuned to %.4f MHz; %s", hz / 1e6,
                     why && *why ? why : "receiver would not start");
            return;
        }
    }
    go_read();
    ls_fresh_bump(&handoff);
    snprintf(feedback, sizeof(feedback), "Listening on %.4f MHz, from %s", hz / 1e6, listen_from);
}

/* ZOOM IN: look closer, a narrow sweep centred on it, where nearly every
   burst lands. Asked mid-sweep, the running one is stopped and this one
   starts when it has let go, as BAND PICK does - zoom_next carries it. */
static uint32_t zoom_next;
static void zoom_start(uint32_t hz)
{
    if (rec_watch_scan_busy()) {
        sweep_stop();
        zoom_next = hz;
        snprintf(feedback, sizeof(feedback), "Zooming in on %.4f MHz...", hz / 1e6);
        return;
    }
    zoom_next = 0;
    const uint32_t half = 150000u;
    if (rec_watch_request_scan(hz > half ? hz - half : hz, hz + half, 0, 8)) {
        scan_stop_asked = false;
        LS_SNPRINTF(t_scr_subghz_sweeping_4f_mhz_150, feedback, sizeof(feedback), "Sweeping %.4f MHz +/-150 kHz", hz / 1e6);
    } else snprintf(feedback, sizeof(feedback), "The receiver would not start a sweep");
    sweep_hits = false;
}

static void detection_done(int index)
{
    if (index < 0 || index >= menu_n || !det_selected_hz) return;
    const uint32_t hz = det_selected_hz;
    switch (menu_id[index]) {
    case 'C':
        listen_here(hz);
        break;
    case 'E':
        ls_catch_save_selected();
        break;
    case 'T':
        if (rec_watch_scan_busy() || rec_watch_enabled()) {
            LS_SNPUTS(t_scr_subghz_stop_the_sweep_or, feedback, sizeof(feedback), "Stop the sweep or WATCH before tuning");
            return;
        }
        if(!rec_set_freq(hz)) {
            LS_SNPUTS(t_scr_subghz_frequency_refused_by_selected, feedback, sizeof(feedback), "Frequency refused by selected receiver");return;
        }
        source_frequency[rec_watch_source()]=hz;
        sweep_hits = false;
        snprintf(feedback, sizeof(feedback), "Tuned to %.4f MHz", hz / 1e6);
        break;
    case 'N':
        zoom_start(hz);
        break;
    case 'F':
        ls_freqs_offer(hz, "OOK", NULL, "subghz", feedback, sizeof(feedback));
        break;
    default: break;
    }
}
/* One menu for anything BAND points at - a detection in the list, a column
   of the chart, a burst CATCH spotted - with LISTEN HERE first, because
   listening is what finding it was for. */
typedef enum { BAND_DETECTION, BAND_PEAK, BAND_BURST } band_kind_t;
static void band_menu_open(band_kind_t kind, uint32_t hz, const char *about)
{
    char title[40];
    snprintf(title, sizeof(title), "%s %.4f MHz",
             kind == BAND_PEAK ? "PEAK" : kind == BAND_BURST ? "BURST" : "DETECTION", hz / 1e6);
    det_selected_hz = hz;
    menu_n = 0;
    ls_picker_open(title, detection_done);
    /* Listening is what turns a detection into something that can be
       looked at, replayed or sent: a sweep only knows that energy was
       there. */
    menu_add('C', "LISTEN HERE", "Tune READ to it, start WATCH");
    menu_add('F', "SAVE TO FREQS", "Keep it in FREQS, mine.csv");
    if (kind == BAND_BURST) {
        menu_add('E', "SAVE RAW", "This burst's samples to the card");
        return;
    }
    menu_add('T', "TUNE HERE", about && about[0] ? about : "Point the receiver at it");
    menu_add('N', "ZOOM IN", LS_TEXT(t_scr_subghz_sweep_150_khz_around, "Sweep +/-150 kHz around it"));
}
static void detection_open(uint32_t hz)
{
    static EXT_RAM_BSS_ATTR rec_scan_bin_t bins[REC_SCAN_BINS];
    int n = rec_watch_scan_result(bins, REC_SCAN_BINS);
    const rec_scan_bin_t *b = NULL;
    for (int i = 0; i < n; i++) if (bins[i].hz == hz) b = &bins[i];
    char seen[40];
    if (b) LS_SNPRINTF(t_scr_subghz_0f_dbm_peak_seen, seen, sizeof(seen), "%.0f dBm peak, seen %lu times", (double)b->dbm, (unsigned long)b->seen);
    else seen[0] = 0;
    band_menu_open(BAND_DETECTION, hz, seen);
}
/* A column of the live chart: the peak, or any other that caught the eye. */
static void peak_open(int column)
{
    if (column < 0 || column >= chart_n) return;
    char about[40];
    snprintf(about, sizeof(about), "%.0f dBm in this sweep", (double)chart_dbm[column]);
    band_menu_open(BAND_PEAK, chart_hz[column], about);
}
/* The loudest column right now, for ENTER on a chart with no detections. */
static int peak_column(void)
{
    int best = -1;
    for (int i = 0; i < chart_n; i++)
        if (best < 0 || chart_dbm[i] > chart_dbm[best]) best = i;
    return best;
}

static void scan_result_done(int index)
{
    rec_scan_bin_t bins[REC_SCAN_BINS];
    const int n=rec_watch_scan_result(bins,REC_SCAN_BINS);
    if(index<0 || index>=n)return;
    if(rec_watch_enabled()){snprintf(feedback,sizeof(feedback),"Stop WATCH first");return;}
    if(!rec_set_freq(bins[index].hz)) {
        LS_SNPUTS(t_scr_subghz_frequency_refused_by_selected, feedback, sizeof(feedback), "Frequency refused by selected receiver");return;
    }
    source_frequency[rec_watch_source()]=bins[index].hz;
    LS_SNPRINTF(t_scr_subghz_tuned_to_4f_mhz, feedback,sizeof(feedback),"Tuned to %.4f MHz (%.0f dBm)",
             bins[index].hz/1e6,(double)bins[index].dbm);
}
static void scan_results_open(void)
{
    rec_scan_bin_t bins[REC_SCAN_BINS];
    const int n=rec_watch_scan_result(bins,REC_SCAN_BINS);
    const float floor_dbm=rec_watch_scan_floor();
    ls_picker_open("SCAN RESULTS",scan_result_done);
    ls_picker_empty_reason(LS_TEXT(t_scr_subghz_no_sweep_yet_run, "No sweep yet - run SCAN first"));
    for(int i=0;i<n && i<12;i++) {
        char label[24],detail[20];
        snprintf(label,sizeof(label),"%.4f MHz",bins[i].hz/1e6);
        /* Against the floor, not as an absolute: -95 dBm means nothing on
           its own and everything when the rest of the band is at -120. */
        snprintf(detail,sizeof(detail),"%.0f dBm  +%.0f",
                 (double)bins[i].dbm,(double)(bins[i].dbm-floor_dbm));
        ls_picker_add(label,detail);
    }
}
/* Where SCAN can point. `bins` is the capture probability as much as the
   resolution - see rec_watch_request_scan. A wide sweep is for finding
   something and can afford to miss bursts; the narrow one is for watching a
   transmitter already found, and must not. The first six are every radio's;
   the rest are only offered to a part that sweeps there (the LR2021), and
   the radio is asked, not assumed. */
static const struct { const char *name,*detail; uint32_t lo,hi; int bins; } SCAN_AT[]={
    {"Watch this frequency","+/-150 kHz, every burst",0,0,8},
    {"Hunt around here","+/- 1 MHz",0,1,32},
    {"433 MHz","433.05 - 434.79",433050000u,434790000u,32},
    {"868 MHz","862 - 870",862000000u,870000000u,32},
    {"915 MHz","902 - 928",902000000u,928000000u,32},
    {"VHF","150 - 170",150000000u,170000000u,32},
    {"315 MHz","310 - 320",310000000u,320000000u,32},
    {"Aviation","960 - 1100",960000000u,1100000000u,32},
    {"GPS L1","1570 - 1581",1570000000u,1581000000u,32},
    {"Iridium","1616 - 1626.5",1616000000u,1626500000u,32},
    {"2.4 GHz ISM","2400 - 2483.5",2400000000u,2483500000u,32},
};
#define SCAN_AT_N ((int)(sizeof(SCAN_AT)/sizeof(SCAN_AT[0])))
/* Picker row -> SCAN_AT entry, for the list as it was last opened. */
static EXT_RAM_BSS_ATTR int scan_at_row[SCAN_AT_N];
static int scan_at_rows;
/* A band picked while a sweep is still running: the running one is asked to
   stop, and this one starts when it has - BAND PICK is never "stop it
   first". -1 for none. */
static int scan_next=-1;
static void scan_start(int index)
{
    iq_view_stop();
    if(index<0 || index>=SCAN_AT_N)return;
    if(rec_watch_scan_busy()) {
        sweep_stop();
        scan_next=index;
        snprintf(feedback,sizeof(feedback),"Changing to %s...",SCAN_AT[index].name);
        return;
    }
    scan_next=-1;
    uint32_t lo=SCAN_AT[index].lo,hi=SCAN_AT[index].hi;
    const int bins=SCAN_AT[index].bins;
    if(!lo && hi<=1) {
        const uint32_t f=rec_get_freq();
        /* Tight for the watch case, a megahertz either side to hunt. */
        const uint32_t half=hi?1000000u:150000u;
        lo=f>half?f-half:150000000u;
        hi=f+half;
    }
    /* Ten seconds, because a transmitter that only speaks when somebody
       presses a button is absent from any shorter look. */
    /* 0 seconds: keep sweeping until it is stopped. Nothing is written down
       until a bin actually crosses the threshold, so leaving it running
       costs nothing but the radio. */
    /* The sweep and WATCH want the same radio, and CATCH the LoRa chip as
       well, so picking a band lets go of whichever was running - the band
       was the answer to "stop it?". */
    const bool was_watching=rec_watch_enabled();
    if(was_watching)watch_set(false);
    if(band_catch || ls_catch_running()){ls_catch_stop();}
    band_catch=false;
    /* WATCH is the usual reason and the only one the operator can act on,
       but it is not the only one - the receiver can simply refuse. Saying
       "stop WATCH" to somebody who is not watching sends them looking for a
       button that is already off. */
    if(!rec_watch_request_scan(lo,hi,0,bins)) {
        snprintf(feedback,sizeof(feedback),"%s",
                 rec_watch_enabled()?"Stop WATCH before scanning":
                                     "The receiver would not start a sweep");
        /* Nothing took the radio, so give WATCH back what it had. */
        if(was_watching && !rec_watch_enabled())watch_set(true);
    } else {
        /* A new sweep opens on the band, not on what the last one found. */
        sweep_hits=false;det_sel=0;det_free=false;band_mark_hz=0;scan_stop_asked=false;
        /* Short enough for the portrait line: SCAN already reads STOP. */
        LS_SNPRINTF(fmt_scr_subghz_sweeping_mhz_bins, feedback,sizeof(feedback),"Sweeping %.3f-%.3f MHz, %d bins",
                 lo/1e6,hi/1e6,bins);
        if(was_watching)snprintf(feedback+strlen(feedback),sizeof(feedback)-strlen(feedback),", WATCH off");
    }
}
static void scan_range_done(int index)
{
    if(index<0 || index>=scan_at_rows)return;
    scan_start(scan_at_row[index]);
}
static void scan_open(void)
{
    const uint32_t caps=ls_lora_caps();
    ls_picker_open("SCAN WHERE",scan_range_done);
    scan_at_rows=0;
    for(int i=0;i<SCAN_AT_N;i++) {
        if(i>=6 && (!(caps&LS_LORA_CAP_RX_WIDE) ||
                    !ls_lora_rx_range_ok(caps,SCAN_AT[i].lo,SCAN_AT[i].hi)))continue;
        scan_at_row[scan_at_rows++]=i;
        ls_picker_add(SCAN_AT[i].name,SCAN_AT[i].detail);
    }
}

/* LEARN, in place.
 *
 * It had a page with LISTEN on it, then a WHAT IT HEARD list, then APPLY -
 * three stops to get from "I don't know how this talks" to a receiver set
 * up to hear it, and WATCH had to be stopped by hand before the first and
 * started again after the last. Now it is one press on READ: WATCH steps
 * aside, the banner counts down and says to press the remote, and when the
 * measurement ends what it found is applied and WATCH comes back.
 *
 * Applying without asking is safe only because it can be taken back: the
 * setup it replaced is kept, and Z (or the feedback line, or OPTIONS) puts
 * it back. */
static bool learn_undo_ok;
static rec_fsk_mod_t learn_undo_fsk;
static bool learn_undo_lr;
static ls_fresh_t learned_flash;
static tui_rect feedback_hit;
static bool feedback_undo;      /* the feedback line is LEARN's result */

static void learn_toggle(void)
{
    if(rec_watch_learn_busy()) {
        rec_watch_scan_stop();
        snprintf(feedback,sizeof(feedback),"Stopping LEARN...");
        return;
    }
    const bool was_watching=rec_watch_enabled();
    if(was_watching && !watch_set(false)) {
        snprintf(feedback,sizeof(feedback),"Could not pause WATCH to learn");
        return;
    }
    /* Thirty seconds: a quarter measuring the tone separation, the rest
       trying seven bit rates. Every one of them needs the transmitter to be
       transmitting, so the instruction matters as much as the button. */
    if(!rec_watch_request_learn(LEARN_SECONDS)) {
        if(was_watching)watch_set(true);
        LS_SNPUTS(t_scr_subghz_learn_refused_radio_busy, feedback, sizeof(feedback), "LEARN refused - the radio is busy");
        return;
    }
    learn_mine=true;learn_resume=was_watching;
    learn_since=esp_timer_get_time();
    feedback_undo=false;
    feedback[0]=0;
}

/* The job ended. What it found goes straight into the receive settings,
   with the old ones kept for Z; a learn that found nothing says so and
   changes nothing. Either way WATCH comes back if LEARN took it. */
static void learn_finish(const char *result)
{
    const bool resume=learn_resume;
    learn_mine=false;learn_resume=false;learn_since=0;
    rec_learn_t r;
    if(rec_watch_learn_result(&r)) {
        rec_fsk_mod_t before;rec_watch_fsk_get(&before);
        const bool lr_before=rec_watch_lr_fsk();
        if(rec_watch_learn_apply()) {
            /* The LR2021 reads OOK off its I/Q until told otherwise; what
               LEARN measured is for its FSK packet receiver. */
            if(ls_lora_chip()==LS_LORA_CHIP_LR20XX)rec_watch_set_lr_fsk(true);
            learn_undo_fsk=before;learn_undo_lr=lr_before;learn_undo_ok=true;
            /* With its confidence: a number offered without one gets
               treated as fact. The rest of what it measured is in the
               list legend (I). */
            rec_fsk_mod_t now;rec_watch_fsk_get(&now);
            LS_SNPRINTF(t_scr_subghz_learned_lu_bd_1fk_undo, feedback,sizeof(feedback),"Learned %lu bd %.1fk %08lX %u%%  Z undo",
                     (unsigned long)now.bitrate,now.deviation_hz/1000.0,(unsigned long)now.sync_word,
                     (unsigned)r.confidence);
            feedback_undo=true;
            ls_fresh_bump(&learned_flash);
        } else LS_SNPUTS(t_scr_subghz_learned_but_the_radio, feedback, sizeof(feedback), "Learned, but the radio refused the settings");
    } else {
        snprintf(feedback,sizeof(feedback),"%s",result && result[0]?result:"LEARN heard nothing it could use");
        feedback_undo=false;
    }
    if(resume && !rec_watch_enabled())watch_set(true);
}

/* Put back the setup LEARN replaced. */
static void learn_undo(void)
{
    if(!learn_undo_ok){snprintf(feedback,sizeof(feedback),"Nothing learned to undo");return;}
    if(rec_watch_learn_busy()){snprintf(feedback,sizeof(feedback),"Stop LEARN first");return;}
    const bool was_watching=rec_watch_enabled();
    if(was_watching)watch_set(false);
    const bool ok=rec_watch_fsk_set(&learn_undo_fsk);
    if(ok && ls_lora_chip()==LS_LORA_CHIP_LR20XX)rec_watch_set_lr_fsk(learn_undo_lr);
    if(was_watching)watch_set(true);
    feedback_undo=false;
    if(!ok){snprintf(feedback,sizeof(feedback),"The radio refused the old settings");return;}
    learn_undo_ok=false;
    LS_SNPRINTF(t_scr_subghz_back_to_lu_bd, feedback,sizeof(feedback),"Back to %lu bd %.0fk sync %08lX",
             (unsigned long)learn_undo_fsk.bitrate,learn_undo_fsk.deviation_hz/1000.0,
             (unsigned long)learn_undo_fsk.sync_word);
    ls_fresh_bump(&learned_flash);
}
/* Pick from a list rather than cycle.

   Cycling meant pressing a menu item five times to see five options and
   remembering which you had passed. Style and colour are also separate
   questions - ASCII versus bars is about legibility, the palette is about
   what the numbers mean - so they get separate lists rather than one
   combined setting that hides both. */
static void style_done(int index)
{
    if (index < 0 || index >= SCAN_GRAINS) return;
    scan_grain = index;
    settings_set_subghz_style(index == SCAN_GRAIN_WATERFALL ? SCAN_GRAIN_WATERFALL_STORED : index);
    snprintf(feedback, sizeof(feedback), "Style: %s", SCAN_GRAIN_NAME[index]);
}
static void style_open(void)
{
    static const char *const WHY[SCAN_GRAINS] = {
        "Eighth-height, smoothest tops",
        "Text ramp, no glyphs needed",
        "A row a pass, colour by dB over the floor"
    };
    ls_picker_open("SPECTRUM STYLE", style_done);
    for (int i = 0; i < SCAN_GRAINS; i++)
        ls_picker_add(SCAN_GRAIN_NAME[i],
                      i == scan_grain ? "in use" : WHY[i]);
}
static void colour_done(int index)
{
    if (index < 0 || index >= SCAN_LOOKS) return;
    scan_look = index;
    settings_set_subghz_colour(index);
    snprintf(feedback, sizeof(feedback), "Colour: %s", SCAN_LOOK_NAME[index]);
}
static void colour_open(void)
{
    static const char *const WHY[SCAN_LOOKS] = {
        "Five steps, dB over floor",
        "Blue to white, wide range",
        "Calm, readable outdoors",
        "One green, intensity only",
        "Magenta to cyan, loud"
    };
    ls_picker_open("SPECTRUM COLOUR", colour_done);
    for (int i = 0; i < SCAN_LOOKS; i++)
        ls_picker_add(SCAN_LOOK_NAME[i],
                      i == scan_look ? "in use" : WHY[i]);
}

/* The third is LISTEN HERE done for you on the first hit: the sweep stops,
   READ is tuned to it and WATCH starts, and the screen follows to READ.
   BAND shows it ARMED while a sweep runs with it set, because a sweep that
   is going to stop by itself should say so before it does. */
static const char *const ON_HIT_NAME[]={"NOTHING","BUZZ","BUZZ + LISTEN"};
static void on_hit_done(int index)
{
    if(index<0 || index>2)return;
    rec_watch_scan_on_hit((rec_scan_on_hit_t)index);
    snprintf(feedback,sizeof(feedback),"On detect: %s",ON_HIT_NAME[index]);
}
static void on_hit_open(void)
{
    const int now=(int)rec_watch_scan_on_hit_get();
    static const char *const WHY[]={
        "Just add it to the list",
        "Vibrate, and DM if ALERTS is on",
        "Vibrate, then LISTEN HERE on READ"
    };
    ls_picker_open("ON DETECTION",on_hit_done);
    for(int i=0;i<3;i++)
        ls_picker_add(ON_HIT_NAME[i],i==now?"in use":WHY[i]);
}

/* The bounds are the runtime's own, checked here so a refusal can say what
   was wrong instead of silently clamping a typed number to something else. */
static void scan_threshold_set(double db)
{
    if(!isfinite(db) || db<REC_SCAN_DETECT_MIN || db>REC_SCAN_DETECT_MAX) {
        LS_SNPRINTF(t_scr_subghz_enter_0f_0f_db, feedback,sizeof(feedback),"Enter %.0f-%.0f dB over the floor",
                 (double)REC_SCAN_DETECT_MIN,(double)REC_SCAN_DETECT_MAX);
        return;
    }
    rec_watch_scan_set_threshold((float)db);
    LS_SNPRINTF(t_scr_subghz_detect_above_0f_db, feedback,sizeof(feedback),"Detect above %.0f dB over the floor",
             (double)rec_watch_scan_threshold());
}
/* Which receiver is listening: the RADIO list every app shows, with the
   three capture sources ready when they are fitted and every other radio
   saying why it cannot. Choosing the LoRa chip takes the mesh down while
   WATCH runs; the banner says so. REC's RADIO comes here for the same three,
   so each keeps its own frequency whichever list chose it. */
bool ls_scr_subghz_choose_source(ls_rsel_radio_t radio)
{
    const rec_source_t want=rec_source_of_radio(radio);
    const rec_source_t now=rec_watch_source();
    if(want>=REC_SOURCE_COUNT)return false;
    if(want==now){ls_rsel_set(LS_RSEL_SUBGHZ_READ,radio);return true;}
    if(rec_watch_enabled() || rec_watch_scan_busy() || rec_watch_learn_busy() || rec_mine || rec_watch_replay_status(NULL,0)) {
        ls_rsel_set(LS_RSEL_SUBGHZ_READ,rec_source_radio(now));
        LS_SNPRINTF(t_scr_subghz_stop_s_before_changing, feedback,sizeof(feedback),"Stop %s before changing source",
            rec_watch_enabled()?"WATCH":rec_watch_scan_busy()?"SCAN":rec_watch_learn_busy()?"LEARN":rec_mine?"RECORD":"replay");
        return false;
    }
    source_frequency[now]=rec_get_freq();
    rec_disarm();
    if(rec_watch_select_source(want)) {
        ls_rsel_set(LS_RSEL_SUBGHZ_READ,radio);
        if(!rec_set_freq(source_frequency[want]))rec_set_freq(433920000u);
        LS_SNPRINTF(t_scr_subghz_s_selected_watch_starts, feedback,sizeof(feedback),"%s selected; WATCH starts capture",source_name());
        return true;
    }
    ls_rsel_set(LS_RSEL_SUBGHZ_READ,rec_source_radio(now));
    snprintf(feedback,sizeof(feedback),"%s would not start",ls_rsel_name(radio));
    return false;
}
static void source_done(ls_rsel_radio_t radio) { (void)ls_scr_subghz_choose_source(radio); }
static void source_open(void) { ls_rsel_open(LS_RSEL_SUBGHZ_READ,source_done); }

/* BAND's radio. Only the LoRa chip sweeps there, so the list is there to
   say so for the rest; choosing it changes nothing. */
static void tune_menu_done(int index)
{
    if(index<0 || index>=menu_n)return;
    const char id=menu_id[index];
    if(id=='T'){ls_numpad_open("WATCH FREQUENCY","MHz",rec_get_freq()/1e6,tune);return;}
    if(id=='V'){scan_results_open();return;}
    if(id>='0' && id<'0'+(int)(sizeof(bands)/sizeof(bands[0])))tune(bands[id-'0']);
}
static void tune_open(void)
{
    menu_n=0;
    ls_picker_open("TUNE",tune_menu_done);
    menu_add('T',"TYPE A FREQUENCY","24 - 1766 MHz");
    for(int j=0;j<(int)(sizeof(bands)/sizeof(bands[0]));j++) {
        char label[32];snprintf(label,sizeof(label),"%.4f MHz",bands[j]);
        /* What is there, not what the receiver can do with it - the old
           "OOK only; no FSK" was a statement about the CC1101 printed
           underneath every frequency whatever was listening. */
        static const char *const WHAT[]={
            "VHF business","VHF business","ISM 315","ISM 433","ISM 868","ISM 915"};
        menu_add((char)('0'+j),label,WHAT[j]);
    }
    if(rec_watch_scan_result(NULL,0)>0)
        menu_add('V',"FROM THE LAST SWEEP",LS_TEXT(t_scr_subghz_tune_to_a_peak, "Tune to a peak it found"));
}

/* THREE PLACES, AND THE JOBS WHERE THEY ARE USED.

   This screen used to open on five buttons, a list and a MORE menu, and the
   parts of the job that were not on the bar lived in other apps. Then it
   became six tabs, one per job - READ, SCAN, SAVED, RAW, LEARN, CATCH - and
   that was compartments instead: measuring a transmitter meant leaving the
   list of what it sent, and finding a frequency ended with copying it down
   and walking back to READ to type it in.

   So there are three places, the way a Flipper has Read, the analyser and
   Saved, and the jobs live where they are used. LEARN and RECORD run on
   READ, in place; the sweep and CATCH are one page, BAND, whose answer is
   LISTEN HERE, which tunes READ and goes there; SAVED is what was kept.
   Nothing that could be done before is gone - see docs/apps/SUBGHZ.md for
   where each of the old tabs' controls went. */
typedef enum {
    PG_READ,       /* listen on one frequency; LEARN and RECORD run here  */
    PG_BAND,       /* find the frequency: the sweep, or CATCH             */
    PG_SAVED,      /* what was caught, and the .sub files on the card     */
    PG_PLAYER,     /* one file, its pulses, and PLAY ONCE - from SAVED    */
    PG_RECORD,     /* REC's level meter and ARM, full size - from READ    */
} sg_page_t;
static sg_page_t page;
static char hint_text[80];
/* What each button on the current page's bar does, set while drawing, so a
   tap and ENTER on a focused button act on what was on the screen. */
static char bar_keys[8];
static int bar_n;

static void go(sg_page_t p);
static void go_read(void) { go(PG_READ); }
static bool on_band(void) { return page == PG_BAND; }
/* A recording landed since SAVED was last opened: a dot on its tab. */
static bool saved_new;
static ls_fresh_t recorded_flash;

/* REC hosts this screen and owns the radio list (it also records GPS and
   mesh metadata) and FILES. Standalone SUB-GHZ leaves both NULL. */
static void (*host_radio)(void);
static void (*host_files)(void);
void ls_scr_subghz_host(void (*radio)(void), void (*files)(void))
{ host_radio = radio; host_files = files; }

/* SAVED: the caught patterns first, then the .sub files on the card. */
#define SAVED_FILES_MAX 48
typedef struct { char path[112]; char name[48]; uint32_t size; long mtime; } saved_file_t;
static EXT_RAM_BSS_ATTR saved_file_t saved_file[SAVED_FILES_MAX];
static int saved_files, saved_sel;
static bool saved_truncated;
static tui_rect saved_list;
static int saved_first;

static bool is_sub(const char *name)
{
    const size_t n=strlen(name);
    return n>4 && (!strcmp(name+n-4,".sub") || !strcmp(name+n-4,".SUB"));
}
static int saved_cmp(const void *a,const void *b)
{
    const saved_file_t *x=a,*y=b;
    if(x->mtime!=y->mtime)return x->mtime<y->mtime?1:-1;
    return strcmp(y->name,x->name);
}
static void saved_scan_dir(const char *dir)
{
    if(!dir || !*dir)return;
    DIR *d=opendir(dir);
    if(!d)return;
    struct dirent *e;
    while((e=readdir(d))!=NULL) {
        if(e->d_name[0]=='.' || !is_sub(e->d_name))continue;
        /* A path that does not fit could not be opened again; skip it
           rather than list a name that leads nowhere. */
        static saved_file_t c;
        if(strlen(dir)+1+strlen(e->d_name)>=sizeof(c.path))continue;
        memcpy(c.path,dir,strlen(dir));c.path[strlen(dir)]='/';
        strcpy(c.path+strlen(dir)+1,e->d_name);
        snprintf(c.name,sizeof(c.name),"%.47s",e->d_name);
        struct stat st;
        if(stat(c.path,&st)==0){c.size=(uint32_t)st.st_size;c.mtime=(long)st.st_mtime;}
        else {c.size=0;c.mtime=0;}
        /* Full: the newest 48 across every folder, not the first 48 the
           card happens to return. Stopping at the first 48 hid a whole
           folder, and the file just copied on with it. */
        if(saved_files<SAVED_FILES_MAX){saved_file[saved_files++]=c;continue;}
        saved_truncated=true;
        int old=0;
        for(int i=1;i<saved_files;i++)if(saved_cmp(&saved_file[i],&saved_file[old])>0)old=i;
        if(saved_cmp(&c,&saved_file[old])<0)saved_file[old]=c;
    }
    closedir(d);
}
/* Newest first, from where REC saves and where WATCH exports. */
static void saved_reload(void)
{
    saved_files=0;saved_truncated=false;
    const char *rec=rec_dir();
    saved_scan_dir(rec);
    if(!rec || strcmp(rec,"/sdcard/subghz"))saved_scan_dir("/sdcard/subghz");
    qsort(saved_file,(size_t)saved_files,sizeof(saved_file[0]),saved_cmp);
}
static int saved_total(void){return s.count+saved_files;}

/* A .sub from the card, into the player. The buffers are this screen's;
   the player keeps its own bounded preview. */
#define PLAYER_EDGES (LS_TX_UNLOCKED ? 65536 : 8192)
static EXT_RAM_BSS_ATTR subghz_file_t player_sub;
static EXT_RAM_BSS_ATTR int32_t player_edges[PLAYER_EDGES];
static EXT_RAM_BSS_ATTR char player_line[SUBGHZ_FILE_LINE_BYTES];
static bool saved_load(int i)
{
    if(i<0 || i>=saved_files)return false;
    if(!subghz_file_load(saved_file[i].path,&player_sub,player_edges,PLAYER_EDGES,
                         player_line,sizeof(player_line))) {
        snprintf(feedback,sizeof(feedback),"Could not read %s",saved_file[i].name);
        return false;
    }
    if(!ls_rec_player_load(saved_file[i].path,&player_sub,player_edges)) {
        const char *why=subghz_tx_refusal(player_sub.freq_hz,0,
            (size_t)player_sub.edges_total,player_sub.span_us);
        if(why && !ls_rec_player_busy()) {
            snprintf(feedback,sizeof(feedback),"Replay refused: %s",why);
            return false;
        }
        snprintf(feedback,sizeof(feedback),"%s",ls_rec_player_busy()?"A replay is still sending":
                 player_sub.invalid?"Malformed or oversized .sub data":
                 !player_sub.filetype_ok || strcmp(player_sub.protocol,"RAW")?"Replay requires a RAW .sub file":
                 "Unsupported or incomplete modulation preset");
        return false;
    }
    ls_rec_player_set_power(tx_step);
    return true;
}
static void saved_open(void)
{
    if(saved_sel<s.count){pick_at(saved_sel);action('c');return;}
    if(saved_load(saved_sel-s.count))go(PG_PLAYER);
}
static void tx_file(int i)
{
    const char *no=tx_blocked();
    if(no){snprintf(feedback,sizeof(feedback),"%s",no);return;}
    if(!saved_load(i))return;
    go(PG_PLAYER);
    tx_player();
}
static void tx_here(void)
{
    switch(page) {
    case PG_PLAYER: tx_player(); break;
    case PG_SAVED:
        if(!saved_total()){snprintf(feedback,sizeof(feedback),"Nothing saved yet");break;}
        if(saved_sel<s.count){pick_at(saved_sel);tx_capture(picked());}
        else tx_file(saved_sel-s.count);
        break;
    case PG_RECORD:
        saved_reload();
        if(!saved_files){snprintf(feedback,sizeof(feedback),"Nothing recorded yet - ARM, then SEND");break;}
        tx_file(0);
        break;
    case PG_READ:
        if(!s.count){snprintf(feedback,sizeof(feedback),"Nothing captured yet - WATCH, then press the remote");break;}
        tx_capture(picked());
        break;
    default:
        snprintf(feedback,sizeof(feedback),"SEND works on READ and SAVED");
        break;
    }
}
static bool watch_set(bool on)
{
    if(!rec_watch_enable(on))return false;
    if(on && rec_watch_source()==REC_SOURCE_RTL){ls_tui_radio_want("REC");rec_arm_request();}
    else if(!on)rec_disarm();
    return true;
}

/* RECORD, in place: what READ RAW was, without the page.
 *
 * One press arms REC's slicer on the SDR (or the LR2021's I/Q recorder) for
 * the next burst over the trigger; it lands on the card as a .sub, which
 * SAVED lists and SEND sends. The banner shows it armed, the level against
 * the trigger and the edges as they come; a tap on it opens REC's full
 * level view for setting the trigger.
 *
 * WATCH is stopped for it, because a running WATCH re-arms the same slicer
 * for its own list and the card write is skipped while it does. It comes
 * back by itself once the file is on the card: the write runs on REC's
 * worker after the burst ends, so WATCH - which re-arms and clears the
 * capture - waits for the worker to be idle (rec_save_busy) and a moment
 * past the burst. With autosave off the capture is only in memory, waiting
 * for a SAVE, so WATCH stays off and W brings it back. */
/* rec_arm_request only leaves a note while REC's receiver is still coming
   up, so IDLE straight after arming is not the end of anything. It is once
   the slicer has been seen armed, or when it never arms at all. */
static bool rec_seen_armed;
static int64_t rec_since;
static bool rec_resume;          /* WATCH was on when RECORD took the slicer */
static int64_t rec_landed_us;    /* the burst landed; WATCH waits on the write */
#define RECORD_RESUME_US 500000
static void record_resume_now(void)
{
    rec_resume=false;rec_landed_us=0;
    if(!rec_watch_enabled() && watch_set(true)) {
        const size_t n=strlen(feedback);
        /* Only where it still fits a portrait line; the banner says
           LISTENING either way. */
        if(n+10<=40)snprintf(feedback+n,sizeof(feedback)-n,"%sWATCH on",n?"; ":"");
    }
}
#define RECORD_ARM_US 5000000
static void record_adopt(bool iq)
{
    rec_hub_status_t rx;rec_get_hub_status(&rx);
    rec_mine=true;rec_iq=iq;rec_caps0=rx.captures;
    rec_seen_armed=iq || rx.phase==REC_ARMED || rx.phase==REC_CAPTURING;
    rec_since=esp_timer_get_time();
}
static void record_toggle(void)
{
    if(rec_mine) {
        if(rec_iq)rec_watch_iq_stop();
        else rec_disarm();
        rec_mine=false;
        snprintf(feedback,sizeof(feedback),"RECORD stopped");
        if(rec_resume)record_resume_now();
        return;
    }
    const bool was_watching=rec_watch_enabled();
    if(was_watching && !watch_set(false)) {
        snprintf(feedback,sizeof(feedback),"Could not stop WATCH to record");
        return;
    }
    ls_rec_tools_enter();
    const bool iq=rec_watch_source()==REC_SOURCE_SX1262;
    if(iq) {
        const uint32_t hz=rec_get_freq();
        if(!rec_watch_iq_record(hz)) {
            if(!ls_lora_rx_range_ok(ls_lora_caps(),hz,hz))
                LS_SNPUTS(t_scr_rec_i_q_outside_range, feedback, sizeof(feedback), "I/Q: frequency outside the radio's range");
            else LS_SNPUTS(t_scr_rec_i_q_busy, feedback, sizeof(feedback), "I/Q: receiver busy or still loading");
            /* Refused: WATCH as it was. */
            if(was_watching)watch_set(true);
            return;
        }
    } else rec_arm_request();
    record_adopt(iq);
    rec_resume=was_watching;rec_landed_us=0;
    snprintf(feedback,sizeof(feedback),"%s",was_watching?"WATCH off for the recording":"");
}
/* The burst landed, or something else let go of the slicer. */
static void record_poll(const rec_hub_status_t *rx)
{
    /* The file is written: WATCH back, as it was before RECORD. */
    if(!rec_mine && rec_resume && rec_landed_us &&
       esp_timer_get_time()-rec_landed_us>RECORD_RESUME_US &&
       !rec_save_busy() && rx->phase!=REC_CAPTURING)
        record_resume_now();
    if(!rec_mine)return;
    if(rec_iq) {
        char result[112];
        if(rec_watch_iq_status(result,sizeof(result)))return;
        rec_mine=false;
        char packet[64];
        if(rec_watch_iq_packet(packet,sizeof(packet)))
            snprintf(feedback,sizeof(feedback),"Recorded: FSK %s - in SAVED",packet);
        else snprintf(feedback,sizeof(feedback),"%s",result[0]?result:"I/Q recording ended");
        saved_new=true;ls_fresh_bump(&recorded_flash);
        /* The I/Q recorder has written its file by the time it reports. */
        if(rec_resume)record_resume_now();
        return;
    }
    if(rx->phase==REC_ARMED || rx->phase==REC_CAPTURING)rec_seen_armed=true;
    if(rx->captures>rec_caps0 && rx->phase!=REC_CAPTURING) {
        rec_mine=false;
        LS_SNPRINTF(t_scr_subghz_recorded_d_edges_at, feedback,sizeof(feedback),"Recorded %d edges at %.4f MHz, in SAVED",
                 rx->edges,rx->freq_hz/1e6);
        saved_new=true;ls_fresh_bump(&recorded_flash);
        if(rec_resume) {
            if(rec_get_autosave())rec_landed_us=esp_timer_get_time();
            else rec_resume=false;          /* in memory only: keep it for SAVE */
        }
    } else if(rx->phase==REC_IDLE && rec_seen_armed) {
        /* Disarmed from somewhere else: REC's own STOP, or a source change. */
        rec_mine=false;
        if(rec_resume)record_resume_now();
    } else if(!rec_seen_armed && esp_timer_get_time()-rec_since>RECORD_ARM_US) {
        rec_mine=false;
        snprintf(feedback,sizeof(feedback),"RECORD: the receiver never armed");
        if(rec_resume)record_resume_now();
    }
}

static const ls_opt_ctx_t *subghz_options(void);

static void action(char c)
{
    iq_view_stop();
    feedback[0]=0;
    feedback_undo=false;
    /* Say why, then stop. Every branch below also guards itself, so this is
       about telling the operator rather than about safety. */
    {
        const char *no=disabled_reason(c);
        if(no){snprintf(feedback,sizeof(feedback),"%s",no);return;}
    }
    if(c=='b') {
        go(page==PG_PLAYER?PG_SAVED:PG_READ);
    } else if(c=='w') {
        bool on=!rec_watch_enabled();
        if(watch_set(on)) {
            tx_paused_watch=false;
            if(on && page!=PG_READ)go(PG_READ);
        } else {
            rec_watch_snapshot(&s);
            /* The receiver that refused knows why; ask it before falling back
               on a sentence about two other receivers. */
            const char *why=rec_watch_error();
            if(!s.ready) why="Still loading the archive from the card";
            else if(!why || !*why) switch(rec_watch_source()) {
                case REC_SOURCE_CC1101: why="No CC1101 - try PROBE in MIX-RF"; break;
                case REC_SOURCE_RTL:    why=ls_rsel_absent(LS_RSEL_SDR_RTL); break;
                default:                why=NULL; break;
            }
            if(!why) why="Receiver would not start";
            snprintf(feedback,sizeof(feedback),"%s",why);
        }
    } else if(c=='c') {
        const rec_watch_event_t *e=s.count?picked():NULL;
        if(!e){LS_SNPUTS(t_scr_subghz_nothing_captured_yet, feedback, sizeof(feedback), "Nothing captured yet");return;}
        char title[40];
        snprintf(title,sizeof(title),"CAPTURE #%lu",(unsigned long)e->id);
        menu_n=0;
        ls_picker_open(title,capture_menu_done);
        /* Offered only when it can actually be done. A capture from a
           receiver that cannot transmit, or one with no modulation recorded,
           has nothing to send it under. */
        if(!e->bitrate || rec_source_can_replay((rec_source_t)e->source)) {
            char now[40];
            snprintf(now,sizeof(now),"%+d dBm %s, once",tx_dbm(e->source),TX_STEP_NAME(tx_step));
            menu_add('R',"SEND NOW",now);
            menu_add('Q',"SEND AT...",LS_TEXT(t_scr_subghz_send_it_again_pick, "Send it again - pick the power"));
        }
        menu_add('S',"SAVE .SUB",LS_TEXT(t_scr_subghz_flipper_file_on_the, "Flipper file on the card"));
        menu_add('P',e->pinned?"UNPIN":"PIN",e->pinned?"Allow eviction":"Keep it");
        menu_add('J',"JOURNAL",LS_TEXT(t_scr_subghz_mark_with_gps_and, "Mark with GPS and time"));
    } else if(c=='t') {
        tx_here();
    } else if(c=='+') {
        tx_power_step(1,true);
    } else if(c=='-') {
        tx_power_step(-1,false);
    } else if(c=='m' || c=='o') {
        ls_opt_open(subghz_options());
    } else if(c=='i') {notes=!notes;
    } else if(c=='n') {
        /* One press either way: a running sweep holds the radio, and letting
           go of it must never be a menu away. */
        if(page!=PG_BAND)go(PG_BAND);
        if(rec_watch_scan_busy()){sweep_stop();snprintf(feedback,sizeof(feedback),"Stopping sweep...");}
        else scan_open();
    } else if(c=='W') {
        scan_open();
    } else if(c=='T') {
        ls_numpad_open("DETECT ABOVE","dB over floor",
                       (double)rec_watch_scan_threshold(),scan_threshold_set);
    } else if(c=='H') {
        on_hit_open();
    } else if(c=='V') {
        sweep_hits=!sweep_hits;
    } else if(c=='Y') {
        scan_look_load();style_open();
    } else if(c=='K') {
        scan_look_load();colour_open();
    } else if(c=='l') {
        /* LEARN runs on READ, whichever page asked for it: the banner there
           is what counts it down. */
        if(page!=PG_READ)go(PG_READ);
        learn_toggle();
    } else if(c=='z') {
        learn_undo();
    } else if(c=='A') {
        /* What the last LEARN found, again - after an undo, say. Kept as
           LEARN's own apply would: the setup it replaces can be undone. */
        rec_learn_t r;
        if(!rec_watch_learn_result(&r)){LS_SNPUTS(t_scr_subghz_nothing_learned_yet_learn, feedback, sizeof(feedback), "Nothing learned yet - L learns");return;}
        if(rec_watch_learn_busy()){snprintf(feedback,sizeof(feedback),"LEARN is still measuring");return;}
        const bool was=rec_watch_enabled();
        if(was && !watch_set(false)){snprintf(feedback,sizeof(feedback),"Could not pause WATCH");return;}
        learn_resume=was;
        learn_finish(NULL);
    } else if(c=='d') {
        /* RECORD runs on READ too, and its full view comes back to READ. */
        if(page!=PG_READ && page!=PG_RECORD)go(PG_READ);
        record_toggle();
    } else if(c=='v') {
        /* On BAND, V is VIEW, as it always was there; elsewhere it opens
           RECORD's full view. */
        if(page==PG_BAND)sweep_hits=!sweep_hits;
        else go(PG_RECORD);
    } else if(c=='u') {
        /* CHART on BAND: this moment as bars, or the last passes as a fall. */
        scan_look_load();
        style_done(scan_grain==SCAN_GRAIN_WATERFALL?0:SCAN_GRAIN_WATERFALL);
    } else if(c=='O') {
        saved_open();
    } else if(c=='U') {
        saved_reload();
        LS_SNPRINTF(t_scr_subghz_d_file_s_on, feedback,sizeof(feedback),"%d file%s on the card",saved_files,saved_files==1?"":"s");
    } else if(c=='f') tune_open();
    else if(c=='p' && selected<s.count) {
        if(!rec_watch_request_pin(s.event[selected].id,!s.event[selected].pinned))
            snprintf(feedback,sizeof(feedback),"Pin request busy");
    } else if(c=='a') {
        memset(peers,0,sizeof(peers));
        ls_picker_open("DETECTION DM TARGET",peer_done);
        ls_picker_add("OFF",LS_TEXT(t_scr_subghz_no_detection_messages, "No detection messages"));
        ls_mesh_peer_t peer;
        for(int j=0;j<LS_MESH_MAX_PEERS && ls_mesh_peer_at(j,&peer);j++) {
            snprintf(peers[j],sizeof(peers[j]),"%s",peer.id);
            ls_picker_add(peer.name[0]?peer.name:peer.id,LS_TEXT(t_scr_subghz_new_patterns_max_one, "New patterns; max one/min; Mesh TX must be armed"));
        }
    } else if(c=='e' && selected<s.count) {
        if(!rec_watch_request_export(s.event[selected].id)) LS_SNPUTS(t_scr_subghz_export_busy_or_pattern, feedback, sizeof(feedback), "Export busy or pattern unavailable");
    } else if(c=='j' && selected<s.count) {
        const rec_watch_event_t *e=&s.event[selected];
        char title[48],text[480];
        snprintf(title,sizeof(title),"SubGHz pattern #%lu",(unsigned long)e->id);
        /* What actually made the recording, named from its source. */
        LS_SNPRINTF(fmt_scr_subghz_not_a_decoded_device, text,sizeof(text),"%s; not a decoded device identity. "
            "%.4f MHz, %u edges, %.2f ms, %lu observations. "
            "First: boot %08lx uptime %llu ms. Last: boot %08lx uptime %llu ms. "
            "GPS and motion attachment describe this bookmark, not the original capture. "
            "Export the representative from SUB-GHZ for pulse data.",
            e->deviation_hz?"Demodulated 2-FSK bitstream":"OOK timing pattern",
            e->frequency/1e6,e->edges,e->span_us/1000.,
            (unsigned long)e->count,(unsigned long)e->first_boot,(unsigned long long)e->first_ms,
            (unsigned long)e->last_boot,(unsigned long long)e->last_ms);
        size_t used=strlen(text);
        if(s.decoded[selected].repeats)LS_SNPRINTF(t_scr_subghz_ook24_payload_06lx_u, text+used,sizeof(text)-used," OOK24 payload %06lX, %u matching frames.",(unsigned long)s.decoded[selected].value,s.decoded[selected].repeats);
        bool ok=ls_field_mark_radio(title,text,e->source==REC_SOURCE_CC1101?LS_FIELD_CC1101:e->source==REC_SOURCE_SX1262?LS_FIELD_LORA:LS_FIELD_RTL,e->frequency,e->count);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy; try again shortly");
    } else if(c=='r' || c=='R') {
        /* One radio for the whole app. SCAN sweeps with the LoRa chip
           whichever radio READ uses, and says so on its page. */
        if(host_radio)host_radio();else source_open();
    } else if(c=='g') {
        if(host_files)host_files();
    } else if(c=='x') {
#if !LS_LR_EDITION
        /* CATCH is BAND's other way of finding a transmitter, so it starts
           there, and lets go of the sweep and WATCH as SCAN does. */
        if(page!=PG_BAND)go(PG_BAND);
        if(!ls_catch_running()) {
            if(rec_watch_scan_busy())sweep_stop();
            if(rec_watch_enabled())watch_set(false);
            band_catch=true;
        }
        ls_catch_toggle();
#endif
    } else if(c=='h') {
        ls_catch_watch_cycle();
    } else if(c=='E') {
        ls_catch_save_selected();
    } else if(c=='s') {
        if(rec_watch_source()==REC_SOURCE_SX1262){fsk_setup_open();return;}
        ls_picker_open("CAPTURE SETUP",setup_done);
        ls_picker_add("Threshold","0 = automatic");
        ls_picker_add("End gap","Silence to end");
        ls_picker_add("Minimum pulse","Glitch filter");
        ls_picker_add("Maximum capture","4096 edges max");
        ls_picker_add("Minimum edges","Reject fragments");
        ls_picker_add("Restore defaults","Auto / 30ms gap");
    }
}

static void go(sg_page_t p)
{
    if(p!=page) {
        iq_view_stop();
        /* CATCH holds the RTL and the LoRa chip while it runs and its bursts
           are drawn on BAND, so it stops when BAND is left, as it did when
           it was a tab. LEARN and RECORD are READ's and carry on: their
           banner is on every page. */
        if(page==PG_BAND && ls_catch_running())ls_catch_stop();
        feedback[0]=0;feedback_undo=false;
    }
    button_focus=-1;button_slot=0;
    if(p==PG_SAVED && page!=PG_PLAYER){saved_reload();saved_sel=0;saved_free=false;saved_new=false;}
    if(p==PG_RECORD)ls_rec_tools_enter();
    page=p;
    if(p==PG_BAND && !rec_watch_scan_busy() && s.scan_result[0])
        snprintf(feedback,sizeof(feedback),"%s",s.scan_result);
}

/* FREQS hands a frequency over the way BAND does: LISTEN HERE, from
   outside. It is run once WATCH's receiver is ready - opening the app
   starts it loading, and a WATCH asked for before then is refused - or
   after LISTEN_IN_US, to say why not. The entry's mode picks the LR2021's
   receiver: OOK off its I/Q, or 2-FSK. */
static uint32_t listen_in;
static int8_t listen_in_fsk = -1;      /* 1 FSK, 0 OOK, -1 as it is */
static int64_t listen_in_since;
#define LISTEN_IN_US 20000000
bool ls_scr_subghz_listen(uint32_t hz, const char *mode)
{
    for(int i=0;i<ls_app_count();i++)
        if(!strcmp(ls_app_at(i)->id,"subghz")) {
            listen_in=hz;listen_in_since=0;
            listen_in_fsk=!mode?-1:!strcmp(mode,"FSK")?1:!strcmp(mode,"OOK")?0:-1;
            ls_app_open(i);return true;
        }
    return false;
}
static void enter(void)
{
    button_focus=-1;button_slot=0;rec_watch_start();ls_field_start();ls_field_watch(true);ls_catch_enter();
    /* The capture source is the truth for READ's RADIO button. */
    ls_rsel_track(LS_RSEL_SUBGHZ_READ,source_radio);
    feedback[0]=0;page=PG_READ;pick_follow=true;
    if(listen_in)listen_in_since=esp_timer_get_time();
}
/* LEARN needs somebody pressing the transmitter in front of this screen, so
   it stops when the screen is left; what it would have applied is not
   applied, but a WATCH it paused comes back. RECORD is REC's slicer and
   carries on, as it always did. */
static void leave(void)
{
    iq_view_stop();waterfall_free();ls_catch_stop();band_catch=false;
    /* A FREQS hand-off still waiting for the receiver is for this visit. */
    listen_in=0;listen_in_since=0;
    if(rec_watch_learn_busy())rec_watch_scan_stop();
    /* The runtime restores WATCH once the stopping LEARN lets go of the
       radio; one that already ended is restored here. */
    if(learn_mine && learn_resume && !rec_watch_learn_rewatch() && !rec_watch_enabled())
        watch_set(true);
    learn_mine=false;learn_resume=false;learn_since=0;
    ls_field_watch(false);
}

static void waveform(tui_surface *sf,tui_rect a)
{
    ls_panel_box(sf,a,"PULSE INSPECTOR",TUI_CYAN);
    pulse_hit=tui_rect_make(0,0,0,0);pulse_total=0;
    if(selected>=s.count || a.h<9)return;
    const rec_watch_event_t *e=&s.event[selected];
    int n=e->edges;if(n>48)n=48;
    uint32_t low=UINT32_MAX,high=0;
    for(int i=0;i<n;i++) {
        uint32_t d=s.preview[selected][i]<0?-(int64_t)s.preview[selected][i]:s.preview[selected][i];
        pulse_total+=d;if(d<low)low=d;if(d>high)high=d;
    }
    if(!pulse_total)return;
    if(pulse_selected>=n)pulse_selected=0;
    char text[110];
    snprintf(text,sizeof(text),"%s #%lu / %s",ls_rsel_name(rec_source_radio((rec_source_t)e->source)),(unsigned long)e->id,
       s.ism[selected][0]?s.ism[selected]:
       e->end_reason==REC_END_SPAN || e->end_reason==REC_END_EDGES?"TRUNCATED":s.decoded[selected].repeats?"OOK24 VERIFIED":e->count>1?"REPEAT MATCH":"RAW CANDIDATE");
    tui_put_str(sf,a,a.x+2,a.y+1,text,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    int w=a.w-10;if(w<2)return;
    pulse_hit=tui_rect_make(a.x+7,a.y+2,w,4);
    tui_put_str(sf,a,a.x+1,a.y+2,"HIGH",LS_ATTR_DIM);
    tui_put_str(sf,a,a.x+1,a.y+4,"LOW",LS_ATTR_DIM);
    uint64_t end=0;int edge=0,previous=-1,previous_edge=0;
    for(int x=0;x<w;x++) {
        uint64_t t=(uint64_t)x*pulse_total/w;
        while(edge<n-1 && end+(uint64_t)(s.preview[selected][edge]<0?-(int64_t)s.preview[selected][edge]:s.preview[selected][edge])<=t) {
            end+=s.preview[selected][edge]<0?-(int64_t)s.preview[selected][edge]:s.preview[selected][edge];edge++;
        }
        int y=a.y+(s.preview[selected][edge]>0?2:4);
        uint8_t color=TUI_ATTR((edge==pulse_selected?TUI_YELLOW:TUI_GREEN)|TUI_BRIGHT,TUI_BLACK);
        if(previous>=0 && (previous!=y || edge!=previous_edge)) {
            tui_put_char(sf,a,pulse_hit.x+x,a.y+2,'+',color);
            tui_put_char(sf,a,pulse_hit.x+x,a.y+3,'|',color);
            tui_put_char(sf,a,pulse_hit.x+x,a.y+4,'+',color);
        } else tui_put_char(sf,a,pulse_hit.x+x,y,'-',color);
        previous=y;previous_edge=edge;
    }
    LS_SNPRINTF(t_scr_subghz_0_2f_ms_first, text,sizeof(text),"0 -> %.2f ms | first %d/%u edges",pulse_total/1000.,n,e->edges);
    tui_put_str(sf,a,a.x+2,a.y+6,text,LS_ATTR_DIM);
    LS_SNPRINTF(t_scr_subghz_tap_trace_d_s, text,sizeof(text),"Tap trace: #%d %s %lu us",pulse_selected+1,s.preview[selected][pulse_selected]>0?"HIGH":"LOW",(unsigned long)(s.preview[selected][pulse_selected]<0?-(int64_t)s.preview[selected][pulse_selected]:s.preview[selected][pulse_selected]));
    tui_put_str(sf,a,a.x+2,a.y+7,text,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    if(a.h>10) {
        LS_SNPRINTF(t_scr_subghz_range_lu_lu_us, text,sizeof(text),"Range %lu..%lu us / %s",(unsigned long)low,(unsigned long)high,e->end_reason==REC_END_GAP?"GAP end":"LIMIT end");
        tui_put_str(sf,a,a.x+2,a.y+8,text,LS_ATTR_DIM);
    }
    if(a.h>12)tui_put_str(sf,a,a.x+2,a.y+10,"| = transition(s) within one time cell",LS_ATTR_DIM);
    if(a.h>11 && s.decoded[selected].repeats) {
        /* What it is, where that is known, rather than the payload alone. */
        char what[64];
        subghz_pwm_format(&s.pwm[selected], what, sizeof(what));
        if(!what[0]) subghz_nrz_format(&s.nrz[selected], what, sizeof(what));
        if(what[0]) LS_SNPRINTF(t_scr_subghz_s_u_frames_u, text,sizeof(text),"%s / %u frames / %u us unit",what,s.decoded[selected].repeats,s.decoded[selected].unit_us);
        else LS_SNPRINTF(t_scr_subghz_ook24_06lx_u_frames, text,sizeof(text),"OOK24 %06lX / %u frames / %u us unit",(unsigned long)s.decoded[selected].value,s.decoded[selected].repeats,s.decoded[selected].unit_us);
        tui_put_str(sf,a,a.x+2,a.y+9,text,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    }
}

/* The bar along the top of every page: BACK first, always in the same
   place, then that page's own controls. Sized by the shared rule - three rows
   in a wide pane when the labels fit the compact form, the fat form
   otherwise - so no page spends more on buttons than the old screen did. */
static tui_rect tabs;
static int tab_w;
/* The page's body below its bar, where a page change wipes. */
static tui_rect page_body;
static rec_hub_status_t bar_rx_now;
static const rec_hub_status_t *bar_rx;
static uint8_t bar_fresh;
static void banner(tui_surface *sf,tui_rect *a,const rec_hub_status_t *rx,uint8_t fresh);
static void page_bar(tui_surface *sf,tui_rect *a,const ls_btn_t *btn,int n)
{
    /* Every page: the one RADIO, then that page's controls, then OPTIONS,
       on one filled row. No BACK - the tabs along the bottom are the way
       between pages, the way FM and P25 work. */
    ls_btn_t row[8];
    int m=0;
    row[m++]=ls_rsel_button(LS_RSEL_SUBGHZ_READ);
    row[0].key='r';
    row[0].dim=rec_watch_enabled() || rec_watch_scan_busy() || rec_watch_learn_busy() || rec_mine;
    for(int i=0;i<n && m<7;i++) {
        const char k=btn[i].key;
        if(k=='b' || k=='r' || k=='R' || k=='o')continue;
        row[m++]=btn[i];
    }
    const ls_opt_ctx_t *opt=subghz_options();
    if(opt && m<8)row[m++]=ls_opt_button(opt);
    const bool wide=a->w>a->h*2;
    /* Portrait is driven by a thumb: more than four controls take two rows
       rather than shrinking every key to a sliver. */
    const bool two=!wide && m>4 && a->h>=40;
    /* Landscape takes the three-row form only when every label still fits
       on its one line; a cut word is worse than a row. The tabs sit beside
       the bar only when that leaves every key nine columns - a half-width
       split gives the bar the row, and 1-3 still change page. */
    const int need=m*9>40?m*9:40;
    const int bw=wide && tab_w>0 && a->w-tab_w>=need?a->w-tab_w:a->w;
    int h=wide?(ls_btn_compact_fits(tui_rect_make(a->x,a->y,bw,3),row,m)?3:4):two?10:5;
    if(h>a->h-6)h=a->h>9?3:0;
    if(ls_tui_compact())h=1;   /* desk: one-row keys, page tabs beside them */
    if(h<=0)return;
    if(two)ls_btn_bar_raised(sf,tui_rect_make(a->x,a->y,a->w,h),row,m,button_focus);
    else ls_btn_bar_raised_row(sf,tui_rect_make(a->x,a->y,bw,h),row,m,button_focus);
    if(bw<a->w)tabs=tui_rect_make(a->x+bw+1,a->y,a->w-bw-1,h);
    bar_n=m<(int)sizeof(bar_keys)?m:(int)sizeof(bar_keys);
    for(int i=0;i<bar_n;i++)bar_keys[i]=row[i].key;
    a->y+=h;a->h-=h;
    /* The receiver's state under the bar, on every page but the player.
       REC's full view and CATCH carry their own capture state. */
    if(page!=PG_PLAYER && page!=PG_RECORD && !(page==PG_BAND && band_catch) && bar_rx)
        banner(sf,a,bar_rx,bar_fresh);
}

static void banner(tui_surface *sf,tui_rect *a,const rec_hub_status_t *rx,uint8_t fresh)
{
    if(a->h<20)return;
    if (s.enabled && !listening_since) listening_since = esp_timer_get_time();
    if (!s.enabled) listening_since = 0;
    /* Inset by a column each side: the pane's edge columns belong to the
       frame around the screen. */
    state_banner(sf,tui_rect_make(a->x+1,a->y,a->w-2,2),&s,rx,esp_timer_get_time(),fresh);
    a->y+=2;a->h-=2;
}

/* ------------------------------------------------------------------ READ */

static void draw_read(tui_surface *sf,tui_rect a,const rec_hub_status_t *rx,
                      const ls_mixrf_status_t *cc,uint8_t fresh)
{
    const bool cc_source=rec_watch_source()==REC_SOURCE_CC1101;
    char freq[16];snprintf(freq,sizeof(freq),"%.3f",rec_get_freq()/1e6);
    char txv[16];snprintf(txv,sizeof(txv),"%+d dBm",tx_face_dbm());
    const bool sending=tx_busy();
    const bool learning=rec_watch_learn_busy();
    /* WATCH reads PAUSED for as long as something that will hand it back
       holds the radio: a send, or LEARN. */
    const bool paused=tx_paused_watch || (learning && learn_resume);
    /* LEARN where the radio can demodulate FSK, RECORD where it can keep a
       raw copy - each only where it can do something, so the bar never
       offers a key that only explains why not. RADIO and OPTIONS are added
       by page_bar, at the two ends. */
    ls_btn_t btn[8];int nb=0;
    btn[nb++]=(ls_btn_t){"FREQ",freq,'f',false,disabled_reason('f')!=NULL};
    btn[nb++]=(ls_btn_t){"WATCH",paused?"PAUSED":s.enabled?"ON":"OFF",'w',s.enabled || paused,disabled_reason('w')!=NULL};
    btn[nb++]=(ls_btn_t){"SEND",sending?"BUSY":txv,'t',sending,!s.count || (!sending && tx_blocked()!=NULL)};
    btn[nb++]=(ls_btn_t){"POWER",TX_STEP_NAME(tx_step),'+',false,sending};
    if(can_learn())
        btn[nb++]=(ls_btn_t){"LEARN",learning?"STOP":"30 S",'l',learning,disabled_reason('l')!=NULL};
    if(can_record())
        btn[nb++]=(ls_btn_t){"RECORD",rec_mine?"STOP":"ARM",'d',rec_mine,disabled_reason('d')!=NULL};
    /* PREV/NEXT only on a pane too narrow to walk the list another way. */
    if(touch_nav) {
        ls_btn_t bottom[2]={
            {"PREV","PATTERN",',',false,!s.count || pick_pos==0},
            {"NEXT","PATTERN",'.',false,pick_pos+1>=s.count}};
        const int bh=ls_tui_compact()?1:5;
        if(a.h>bh+20) {
            ls_btn_bar_raised_slot(sf,tui_rect_make(a.x,a.y+a.h-bh,a.w,bh),bottom,2,-1,LS_BTN_SLOT_WATERFALL);
            a.h-=bh;
        }
    }
    page_bar(sf,&a,btn,nb);
    tui_rect body=tui_rect_make(a.x,a.y,a.w,a.h-1);
    page_body=body;
    tui_rect content=tui_rect_make(body.x+1,body.y+1,body.w-2,body.h-2);
    char panel_title[48];
    snprintf(panel_title,sizeof(panel_title),"READ / %s %s",source_name(),
             rec_watch_source()==REC_SOURCE_SX1262 && !lr_ook()?"2-FSK":"OOK");
    /* The frame flashes green when LEARN has just set the receiver or a
       recording has just landed: the change is to the whole of READ, not
       to one number on it. */
    const uint8_t learned=ls_fresh_level(&learned_flash,1500);
    const uint8_t recorded=ls_fresh_level(&recorded_flash,1500);
    ls_panel_box(sf,body,panel_title,(learned>=128 || recorded>=128)?TUI_GREEN:TUI_CYAN);
    ls_motion_busy(sf,body,s.exporting || sending || (s.enabled && rx->receiver_streaming));
    tx_gauge(sf,body);
    char line[110];
    if(cc_source && cc->raw_overflows) {
        snprintf(line,sizeof(line),"CC buffer overflows %lu",(unsigned long)cc->raw_overflows);
        tui_put_str(sf,content,body.x+2,body.y+1,line,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    }
    list=tui_rect_make(body.x+2,body.y+2,body.w-4,body.h-4);
    if(body.w>90 && body.h>12) {
        list.w=(body.w-6)/2;
        waveform(sf,tui_rect_make(list.x+list.w+2,body.y+1,body.w-list.w-5,body.h-2));
    } else if(body.h>24) {
        list.h=(body.h-16)/2*2;
        waveform(sf,tui_rect_make(body.x+1,list.y+list.h+1,body.w-2,body.h-list.h-4));
    }
    int rows=list.h/2;if(rows<1)rows=1;
    if(!read_free)read_first=pick_pos/rows*rows;
    if(ord_n>rows && ls_drag_scroll(&read_drag,list,2,&read_first,
                                    drag_span(&read_drag,read_first,ord_n,rows),rows))read_free=true;
    if(read_first>=ord_n)read_first=ord_n-1;
    if(read_first<0)read_first=0;
    const int first=read_first;
    if(!s.count) {
        tui_put_str(sf,content,list.x,list.y,
                    s.enabled?"Listening - press the remote":"Nothing caught yet. Press WATCH.",LS_ATTR_DIM);
        /* What turns up here, while the list has room - and the keys, with
           LEARN and RECORD only where this radio has them. */
        char jobs[48];int at=0;
        if(can_learn())at+=snprintf(jobs+at,sizeof(jobs)-(size_t)at,"%-13s","L  learn");
        if(can_record())at+=snprintf(jobs+at,sizeof(jobs)-(size_t)at,"%-15s","D  record");
        snprintf(jobs+at,sizeof(jobs)-(size_t)at,"2  band");
        const char *const HOW[]={
            "Each burst on the channel becomes a capture:",
            "its timing, a decode when the pattern is known,",
            "and a device name when rtl_433 knows it.",
            "",
            "W  watch      F  frequency    R  radio",
            "T  send it    +/-  power      ENTER  more",
            jobs,
        };
        for(int i=0;i<(int)(sizeof(HOW)/sizeof(HOW[0])) && list.y+2+i<list.y+list.h;i++)
            tui_put_str(sf,list,list.x,list.y+2+i,HOW[i],
                        i>=4?TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK):TUI_ATTR(TUI_WHITE,TUI_BLACK));
    }
    for(int i=0;i<rows && first+i<ord_n;i++) {
        const int k=ord[first+i];
        const rec_watch_event_t *e=&s.event[k];int y=list.y+i*2;
        if(first+i==pick_pos)ls_fill_dither(sf,tui_rect_make(list.x,y,list.w,2),
                                            sending?LS_DITHER_MEDIUM:LS_DITHER_LIGHT,sending?TUI_YELLOW:TUI_CYAN);
        LS_SNPRINTF(t_scr_subghz_s_lu_4fmhz_x, line,sizeof(line),"%s #%lu %.4fMHz x%lu",e->last_boot==s.boot_id?"[RX]":"[SD]",(unsigned long)e->id,e->frequency/1e6,(unsigned long)e->count);
        tui_put_str(sf,list,list.x,y,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        char what[64];
        subghz_pwm_format(&s.pwm[k], what, sizeof(what));
        if(!what[0]) subghz_nrz_format(&s.nrz[k], what, sizeof(what));
        const char *heard=ls_rsel_name(rec_source_radio((rec_source_t)e->source));
        /* A device rtl_433's decoders know comes first: it is an identity,
           the rest are timing patterns. */
        const bool device=s.ism[k][0]!=0;
        if(device)snprintf(line,sizeof(line),"%s %s",heard,s.ism[k]);
        else if(what[0])snprintf(line,sizeof(line),"%s %s / %u repeats",heard,what,s.decoded[k].repeats);
        else if(s.decoded[k].repeats)LS_SNPRINTF(t_scr_subghz_s_ook24_06lx_u, line,sizeof(line),"%s OOK24 %06lX / %u repeats",heard,(unsigned long)s.decoded[k].value,s.decoded[k].repeats);
        else snprintf(line,sizeof(line),"%s %u edges %.1fms",e->end_reason!=REC_END_GAP?"LIMIT":e->count>1?"REPEAT":"RAW?",e->edges,e->span_us/1000.);
        tui_put_str(sf,list,list.x,y+1,line,device?TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM);
    }
    if(notes) {
        tui_rect info=tui_rect_make(body.x+1,body.y+1,body.w-2,body.h-2);
        tui_fill(sf,info,' ',LS_ATTR_DIM);
        ls_panel_box(sf,info,"WHAT THE LIST MEANS",TUI_CYAN);
        tui_put_str(sf,info,info.x+2,info.y+1,s.storage,LS_ATTR_DIM);
        LS_SNPRINTF(t_scr_subghz_loss_lu_skipped_cc, line,sizeof(line),"Loss: %lu skipped / CC buffer overflows %lu",(unsigned long)s.dropped,(unsigned long)cc->raw_overflows);
        tui_put_str(sf,info,info.x+2,info.y+2,line,LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+3,"RAW? unverified / REPEAT timing match",LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+4,"[SD] earlier boot / [RX] this boot",LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+5,"Pattern match is not a device identity.",LS_ATTR_DIM);
        if(cc_source) LS_SNPUTS(t_scr_subghz_cc_650khz_ook_40us, line, sizeof(line), "CC: 650kHz OOK / 40us filter / 30ms gap");
        else if(lr_ook()) LS_SNPRINTF(t_scr_subghz_s_ook_from_its, line,sizeof(line),"%s: OOK from its I/Q stream / 30ms gap",source_name());
        else if(rec_watch_source()==REC_SOURCE_SX1262) LS_SNPRINTF(t_scr_subghz_s_2_fsk_rate, line,sizeof(line),"%s: 2-FSK, rate and deviation in OPTIONS",source_name());
        else LS_SNPRINTF(t_scr_subghz_s_configurable_ook_pulse, line,sizeof(line),"%s: configurable OOK pulse capture",source_name());
        tui_put_str(sf,info,info.x+2,info.y+6,line,LS_ATTR_DIM);
        LS_SNPRINTF(t_scr_subghz_dm_queued_lu_refused, line,sizeof(line),"DM queued %lu / refused %lu / limited %lu",(unsigned long)s.alert_sent,(unsigned long)s.alert_failed,(unsigned long)s.alert_suppressed);
        tui_put_str(sf,info,info.x+2,info.y+7,line,LS_ATTR_DIM);
        /* What LEARN measured last, in full: the rows its own page had. */
        rec_learn_t lr;
        if(rec_watch_learn_result(&lr) && info.h>11) {
            LS_SNPRINTF(t_scr_subghz_last_learn_lu_baud, line,sizeof(line),"LEARN: %lu baud, %.1f kHz dev, sync %08lX",
                     (unsigned long)lr.bitrate,lr.deviation_hz/1000.0,(unsigned long)lr.sync_word);
            tui_put_str(sf,info,info.x+2,info.y+9,line,TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK));
            snprintf(line,sizeof(line),"%u%% confident: %s",lr.confidence,lr.note);
            tui_put_str(sf,info,info.x+2,info.y+10,line,LS_ATTR_DIM);
        }
    }
    /* LEARN's result, a recording landing and a hand-off from BAND are lit
       on arrival and cool to the usual grey; LEARN's stays cyan while its
       UNDO is a tap on it. */
    const uint8_t lit=learned>recorded?learned:recorded;
    const uint8_t hand=ls_fresh_level(&handoff,1500);
    const uint8_t heat=lit>hand?lit:hand;
    const uint8_t fb_at=!feedback[0]?LS_ATTR_DIM:
                        heat>=128?TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK):
                        feedback_undo?TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM;
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:s.export_status[0]?s.export_status:
                 s.count?"UP/DOWN pick, SEND replays it, ENTER more":"WATCH, then press the remote",fb_at);
    feedback_hit=feedback_undo?tui_rect_make(a.x,a.y+a.h-1,a.w,1):tui_rect_make(0,0,0,0);
}

/* ------------------------------------------------------------------ BAND */

/* THE SWEEP AND WHAT IT FOUND, ON ONE PAGE.
 *
 * The chart and the detections were one VIEW key apart, so looking at the
 * list meant losing the band, and the column that jumped was gone by the time
 * its row was found. A portrait pane has the rows for both and a landscape
 * one the columns: the chart and the list share the page, the row picked in
 * the list is marked on the chart, and either answers LISTEN HERE. Only a
 * pane too small for both keeps VIEW.
 *
 * CATCH is BAND's other way of finding something - the RTL spots a burst in
 * a 2.4 MHz slice and the LoRa chip decodes it - so it is here rather than
 * on a page of its own; while it is in use its slice and bursts take the
 * body, and SCAN brings the sweep back. */
static bool band_both(tui_rect inner) { return inner.w>=90 || inner.h>=30; }
static void band_armed(tui_surface *sf,tui_rect body,bool sweeping)
{
    /* What a hit will do, on the frame's bottom edge where READ shows TX:
       a sweep set to hand off says so before it does. */
    const rec_scan_on_hit_t on_hit=rec_watch_scan_on_hit_get();
    if(on_hit==REC_SCAN_ON_HIT_NOTHING || body.w<34)return;
    char chip[32];
    const bool armed=sweeping && on_hit==REC_SCAN_ON_HIT_CATCH;
    if(armed)snprintf(chip,sizeof(chip)," ARMED: 1ST HIT > READ ");
    else snprintf(chip,sizeof(chip)," ON HIT: %s ",on_hit==REC_SCAN_ON_HIT_CATCH?"LISTEN":"BUZZ");
    const int x=body.x+body.w-2-(int)strlen(chip);
    tui_put_str(sf,body,x,body.y+body.h-1,chip,
                armed?TUI_ATTR(TUI_BLACK,TUI_GREEN|TUI_BRIGHT):
                      TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
}
static void draw_band(tui_surface *sf,tui_rect a)
{
    const bool sweeping=rec_watch_scan_busy();
    const bool catching=ls_catch_running();
    ls_rsel_hw_t hw; ls_rsel_hw(&hw);
    bool iq = !sweeping && !band_catch && !rec_watch_enabled() && hw.lora_lr20xx &&
              hw.present[LS_RSEL_LORA] && !hw.present[LS_RSEL_SDR_RTL] &&
              !hw.present[LS_RSEL_SDR_HACKRF];
    if (!iq) iq_view_stop();
    const int found=rec_watch_scan_result(NULL,0);
    char thr[16];snprintf(thr,sizeof(thr),"%.0f dB",(double)rec_watch_scan_threshold());
    /* The body's size decides whether VIEW is needed, before the bar is
       drawn: the bar's height is what decides the body's size. Estimated
       from the pane, which is what the bar is fitted to. */
    const bool small=iq || !band_both(tui_rect_make(a.x,a.y,a.w-4,a.h-20));
    ls_btn_t btn[8];int nb=0;
    btn[nb++]=(ls_btn_t){"SCAN",sweeping?"STOP":"START",'n',sweeping,disabled_reason('n')!=NULL};
    if(!band_catch) {
        btn[nb++]=(ls_btn_t){"BAND","PICK",'W',false,disabled_reason('n')!=NULL && !sweeping};
        btn[nb++]=(ls_btn_t){"DETECT",thr,'T',false,false};
        scan_look_load();
        btn[nb++]=(ls_btn_t){"CHART",scan_grain==SCAN_GRAIN_WATERFALL?"FALLS":"BARS",'u',scan_grain==SCAN_GRAIN_WATERFALL,false};
        if(small)btn[nb++]=(ls_btn_t){"VIEW",sweep_hits?"CHART":"FOUND",'V',sweep_hits,!sweeping && found<=0};
    }
#if !LS_LR_EDITION
    bool watch_on=false;
    const char *watch_label=ls_catch_watch_label(&watch_on);
    btn[nb++]=(ls_btn_t){"CATCH",catching?"STOP":"START",'x',catching,false};
    if(band_catch) {
        btn[nb++]=(ls_btn_t){"WATCH",watch_label,'h',watch_on,false};
        btn[nb++]=(ls_btn_t){"RAW","SAVE",'E',false,false};
    }
#endif
    page_bar(sf,&a,btn,nb);
    tui_rect body=tui_rect_make(a.x,a.y,a.w,a.h-1);
    page_body=body;
    band_mark_hz=0;
#if !LS_LR_EDITION
    if(band_catch) {
        ls_catch_body_draw(sf,body);
        ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"ENTER on a burst: LISTEN HERE",LS_ATTR_DIM);
        return;
    }
#endif
    if (iq && !sweep_hits) {
        s_iq_view = true;
        ls_wf_source_select(LS_WF_SRC_LORA);
        ls_wf_source_view(rec_get_freq());
        ls_wf_source_pump();
        ls_wf_draw_mini(sf, body);
        ls_safe_line(sf,a,a.y+a.h-1, LS_TEXT(t_scr_subghz_lr_iq_125_khz, "LR IQ / 125 kHz   SCAN for wide sweep"), LS_ATTR_DIM);
        return;
    }
    iq_view_stop();
    char title[40];
    snprintf(title,sizeof(title),"BAND / %s SWEEP",ls_rsel_name(LS_RSEL_LORA));
    ls_panel_box(sf,body,title,TUI_CYAN);
    ls_motion_busy(sf,body,sweeping);
    band_armed(sf,body,sweeping);
    if(sweeping)hit_flash_poll();
    const tui_rect inner=tui_rect_make(body.x+2,body.y+1,body.w-4,body.h-2);
    const bool list=found>0 || sweeping;
    if(!list) {
        tui_rect w=tui_rect_make(body.x+2,body.y+2,body.w-4,body.h-4);
        tui_put_str(sf,w,w.x,w.y,"Find a transmitter you do not know the",TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        tui_put_str(sf,w,w.x,w.y+1,"frequency of:",TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        tui_put_str(sf,w,w.x,w.y+3,"1. SCAN, and pick a band",LS_ATTR_DIM);
        tui_put_str(sf,w,w.x,w.y+4,"2. Press the transmitter a few times",LS_ATTR_DIM);
        tui_put_str(sf,w,w.x,w.y+5,"3. Tap a column or a detection:",LS_ATTR_DIM);
        tui_put_str(sf,w,w.x,w.y+6,"   LISTEN HERE tunes READ to it",TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK));
        char where[64];
        LS_SNPRINTF(t_scr_subghz_sweeps_on_the_s, where,sizeof(where),"Sweeps on the %s; mesh pauses meanwhile.",ls_rsel_name(LS_RSEL_LORA));
        tui_put_str(sf,w,w.x,w.y+8,where,LS_ATTR_DIM);
#if !LS_LR_EDITION
        tui_put_str(sf,w,w.x,w.y+10,"CATCH: the RTL-SDR spots bursts in",LS_ATTR_DIM);
        tui_put_str(sf,w,w.x,w.y+11,"2.4 MHz and the LoRa chip decodes them.",LS_ATTR_DIM);
#endif
        ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"SCAN starts a sweep",LS_ATTR_DIM);
        return;
    }
    /* Which detection is picked, so the chart can mark it: the list is drawn
       first for that reason. */
    tui_rect table=inner,chart=tui_rect_make(0,0,0,0);
    if(sweeping && !small) {
        if(inner.w>=90) {
            /* Side by side: the list is its forty-odd columns, the chart
               the rest. */
            table=tui_rect_make(inner.x+inner.w-44,inner.y,44,inner.h);
            chart=tui_rect_make(inner.x,inner.y,inner.w-46,inner.h);
        } else {
            const int ch=inner.h*11/20;
            chart=tui_rect_make(inner.x,inner.y,inner.w,ch);
            table=tui_rect_make(inner.x,inner.y+ch+1,inner.w,inner.h-ch-1);
        }
    } else if(sweeping && !sweep_hits) {
        chart=inner;table=tui_rect_make(0,0,0,0);
    }
    if(table.w>0) {
        hits_table(sf,table);
        if(det_row_n && det_sel>=det_first && det_sel-det_first<det_row_n)
            band_mark_hz=det_row_hz[det_sel-det_first];
    }
    if(chart.w>0)spectrum(sf,chart);
    else {gate_hit=tui_rect_make(0,0,0,0);chart_n=0;}
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:
                 sweeping?"Tap a column or a detection: LISTEN HERE":"ENTER on a detection: LISTEN HERE",
                 LS_ATTR_DIM);
}

/* ----------------------------------------------------------------- SAVED */

static void draw_saved(tui_surface *sf,tui_rect a)
{
    const int total=saved_total();
    if(saved_sel>=total)saved_sel=total?total-1:0;
    const bool on_file=saved_sel>=s.count;
    const bool sending=tx_busy();
    char txv[16];snprintf(txv,sizeof(txv),"%+d dBm",tx_face_dbm());
    ls_btn_t btn[]={
        {"BACK","HOME",'b',false,false},
        {"SEND",sending?"BUSY":txv,'t',sending,!total || (!sending && tx_blocked()!=NULL)},
        {"POWER",TX_STEP_NAME(tx_step),'+',false,sending},
        {"OPEN",!total?"NONE":on_file?"PLAYER":"CAPTURE",'O',false,!total},
        {"RELOAD","CARD",'U',false,false},
        {"FILES","BROWSE",'g',false,false}};
    page_bar(sf,&a,btn,host_files?6:5);
    tui_rect body=tui_rect_make(a.x,a.y,a.w,a.h-1);
    page_body=body;
    ls_panel_box(sf,body,"SAVED",TUI_YELLOW);
    ls_motion_busy(sf,body,sending);
    tx_gauge(sf,body);
    saved_list=tui_rect_make(body.x+2,body.y+1,body.w-4,body.h-2);
    char line[112];
    if(!total) {
        tui_put_str(sf,saved_list,saved_list.x,saved_list.y,"Nothing yet.",TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        tui_put_str(sf,saved_list,saved_list.x,saved_list.y+2,"Captures from READ, recordings from RECORD",LS_ATTR_DIM);
        tui_put_str(sf,saved_list,saved_list.x,saved_list.y+3,"and .sub files on the card show up here.",LS_ATTR_DIM);
        ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"",LS_ATTR_DIM);
        return;
    }
    /* Two lines a row, a heading where the kind changes. */
    const int rows=saved_list.h/2>0?saved_list.h/2:1;
    const int nvis=rows>1?rows-1:1;
    if(total>nvis && ls_drag_scroll(&saved_drag,saved_list,2,&saved_first,total,nvis))saved_free=true;
    if(!saved_free) {
        if(saved_sel<saved_first)saved_first=saved_sel;
        if(saved_sel>=saved_first+nvis)saved_first=saved_sel-nvis+1;
    }
    if(saved_first>total-nvis)saved_first=total-nvis;
    if(saved_first<0)saved_first=0;
    int y=saved_list.y;
    for(int i=saved_first;i<total && y+1<saved_list.y+saved_list.h;i++) {
        if(i==saved_first || i==s.count) {
            const char *head=i<s.count?"CAUGHT - this boot and the archive":
                             saved_truncated?"FILES ON THE CARD - newest 48":"FILES ON THE CARD";
            tui_put_str(sf,saved_list,saved_list.x,y,head,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
            y++;
            if(y+1>=saved_list.y+saved_list.h)break;
        }
        if(i==saved_sel)ls_fill_dither(sf,tui_rect_make(saved_list.x,y,saved_list.w,2),LS_DITHER_LIGHT,TUI_CYAN);
        if(i<s.count) {
            const rec_watch_event_t *e=&s.event[i<ord_n?ord[i]:i];
            LS_SNPRINTF(t_scr_subghz_lu_4f_mhz_x, line,sizeof(line),"#%lu  %.4f MHz  x%lu%s",(unsigned long)e->id,e->frequency/1e6,
                     (unsigned long)e->count,e->pinned?"  PINNED":"");
            tui_put_str(sf,saved_list,saved_list.x,y,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
            LS_SNPRINTF(t_scr_subghz_s_u_edges_1f, line,sizeof(line),"%s  %u edges  %.1f ms",ls_rsel_name(rec_source_radio((rec_source_t)e->source)),
                     e->edges,e->span_us/1000.);
            tui_put_str(sf,saved_list,saved_list.x,y+1,line,LS_ATTR_DIM);
        } else {
            const saved_file_t *f=&saved_file[i-s.count];
            tui_put_str(sf,saved_list,saved_list.x,y,f->name,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
            const char *dir=strrchr(f->path,'/');
            snprintf(line,sizeof(line),"%.*s  %lu bytes",dir?(int)(dir-f->path):0,f->path,(unsigned long)f->size);
            tui_put_str(sf,saved_list,saved_list.x,y+1,line,LS_ATTR_DIM);
        }
        y+=2;
    }
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:
                 on_file?"SEND transmits it, ENTER opens the player":"SEND replays it, ENTER: save .sub, pin, journal",LS_ATTR_DIM);
}

/* Which entry a tap in the list landed on, by redrawing the same walk. */
static int saved_hit(int y)
{
    const int total=saved_total();
    int row=saved_list.y;
    for(int i=saved_first;i<total && row+1<saved_list.y+saved_list.h;i++) {
        if(i==saved_first || i==s.count){row++;if(row+1>=saved_list.y+saved_list.h)break;}
        if(y==row || y==row+1)return i;
        row+=2;
    }
    return -1;
}

/* ---------------------------------------------------------------- PLAYER */

static void draw_player(tui_surface *sf,tui_rect a)
{
    tx_step=ls_rec_player_power();
    ls_btn_t btn[]={{"BACK","SAVED",'b',false,false}};
    page_bar(sf,&a,btn,1);
    page_body=a;
    ls_rec_player_draw(sf,a);
}

/* ---------------------------------------------------------------- RECORD */

/* REC's level meter, rate chart and ARM, full size: for setting the trigger,
   which the banner's one-row meter can show but not explain. It is READ's,
   opened from RECORD's banner chip or V, and BACK returns to READ. */
static void draw_record(tui_surface *sf,tui_rect a)
{
    const bool sending=tx_busy();
    ls_btn_t btn[]={
        {"RECORD",rec_mine?"STOP":"ARM",'d',rec_mine,disabled_reason('d')!=NULL},
        {"SEND",sending?"BUSY":"LAST",'t',sending,!sending && tx_blocked()!=NULL}};
    page_bar(sf,&a,btn,2);
    page_body=a;
    if(feedback[0] && a.h>12) {
        ls_safe_line(sf,a,a.y+a.h-1,feedback,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
        a.h--;
    }
    const char *no=can_record()?NULL:disabled_reason('d');
    if(no) {
        char how[48];
        LS_SNPUTS(t_scr_subghz_tap_radio_and_pick, how, sizeof(how), "Tap RADIO and pick the SDR");
        ls_panel_notice(sf,a,"RECORD",no,how);
        return;
    }
    ls_rec_tools_draw(sf,a);
}

/* -------------------------------------------------------------- SETTINGS */

/* Every setting this app has, with its value beside it. Each row opens the
   same list or keypad it always did; what changed is that they are all here
   instead of spread over a bar, a MORE menu and two sub-menus. */
#define SETTING_ROWS 9
static const char SETTING_KEY[SETTING_ROWS]={'f','r','s','T','H','a','Y','K','i'};
static void settings_value(int row,char *out,size_t len)
{
    rec_fsk_mod_t m;
    switch(row) {
    case 0: snprintf(out,len,"%.4f MHz",rec_get_freq()/1e6); break;
    case 1: { ls_mixrf_status_t cc;ls_mixrf_snapshot(&cc);snprintf(out,len,"%s",source_face(&cc)); } break;
    case 2:
        if(rec_watch_source()==REC_SOURCE_SX1262){rec_watch_fsk_get(&m);snprintf(out,len,"%lu bd %.0fk dev",(unsigned long)m.bitrate,m.deviation_hz/1000.0);}
        else if(rec_watch_source()==REC_SOURCE_CC1101)snprintf(out,len,"fixed");
        else LS_SNPRINTF(t_scr_subghz_gap_d_ms_min, out,len,"gap %d ms, min %d edges",rec_get_gap_ms(),rec_get_min_edges());
        break;
    case 3: snprintf(out,len,"%.0f dB over floor",(double)rec_watch_scan_threshold()); break;
    case 4: snprintf(out,len,"%s",ON_HIT_NAME[(int)rec_watch_scan_on_hit_get()]); break;
    case 5: snprintf(out,len,"%s",s.alerts?"mesh DM on":"off"); break;
    case 6: scan_look_load();snprintf(out,len,"%s",SCAN_GRAIN_NAME[scan_grain]); break;
    case 7: scan_look_load();snprintf(out,len,"%s",SCAN_LOOK_NAME[scan_look]); break;
    default: snprintf(out,len,"%s",notes?"shown":"hidden"); break;
    }
}
static void o_row(const ls_opt_t *o) { action(SETTING_KEY[o->arg]); }
static void o_row_show(const ls_opt_t *o,char *out,size_t n)
{
    if(o->arg==3)snprintf(out,n,"%.0f dB",(double)rec_watch_scan_threshold());
    else settings_value(o->arg,out,n);
}
static const char *o_row_why(const ls_opt_t *o) { return disabled_reason(SETTING_KEY[o->arg]); }
static double o_gate(const ls_opt_t *o) { (void)o;return rec_watch_scan_threshold(); }
static void o_gate_set(const ls_opt_t *o,double v) { (void)o;scan_threshold_set(v); }
#define SG_ROW(name,row) { .label=(name), .kind=LS_OPT_ACTION, .arg=(row), .act=o_row, \
                           .show=o_row_show, .why_not=o_row_why }
/* The LR2021 reads OOK from its I/Q stream by default; FSK packets once
   LEARN has measured a rate and sync. */
static const char *const LR_RX_NAMES[]={"OOK (I/Q)","FSK packet"};
static int o_lr_rx(const ls_opt_t *o){(void)o;return rec_watch_lr_fsk()?1:0;}
static void o_lr_rx_set(const ls_opt_t *o,int v){(void)o;(void)rec_watch_set_lr_fsk(v!=0);}
static const char *o_lr_rx_why(const ls_opt_t *o)
{
    (void)o;
    if(ls_lora_chip()!=LS_LORA_CHIP_LR20XX)return "LR2021 only";
    return rec_watch_enabled()?"Stop WATCH first":NULL;
}
static char tx_opt_text[SUBGHZ_TX_STEPS_MAX][24];
static const char *tx_opt_names[SUBGHZ_TX_STEPS_MAX];
static void tx_opt_refresh(void)
{
    const rec_watch_event_t *e=picked();
    const uint8_t src=e?e->source:(uint8_t)rec_watch_source();
    for(int i=0;i<TX_STEPS;i++) {
        snprintf(tx_opt_text[i],sizeof(tx_opt_text[i]),"%s %+d dBm",TX_STEP_NAME(i),
                 subghz_tx_step_dbm(i,src==REC_SOURCE_CC1101));
        tx_opt_names[i]=tx_opt_text[i];
    }
}
static int o_tx(const ls_opt_t *o){(void)o;return tx_step;}
static void o_tx_set(const ls_opt_t *o,int v){(void)o;if(v>=0 && v<TX_STEPS && !tx_busy()){tx_step=v;ls_rec_player_set_power(v);}}
static const char *o_tx_why(const ls_opt_t *o){(void)o;return tx_busy()?"Wait for the send to finish":NULL;}
static const char *const RESUME_NAMES[]={"STAY STOPPED","RESUME WATCH"};
static int o_resume(const ls_opt_t *o){(void)o;return tx_resume?1:0;}
static void o_resume_set(const ls_opt_t *o,int v){(void)o;tx_resume=v!=0;}
static void o_undo(const ls_opt_t *o){(void)o;action('z');}
static void o_undo_show(const ls_opt_t *o,char *out,size_t n)
{
    (void)o;
    if(learn_undo_ok)snprintf(out,n,"back to %lu bd",(unsigned long)learn_undo_fsk.bitrate);
    else snprintf(out,n,"nothing to undo");
}
static const char *o_undo_why(const ls_opt_t *o){(void)o;return learn_undo_ok?NULL:"Nothing learned to undo";}
static void o_apply(const ls_opt_t *o){(void)o;action('A');}
static void o_apply_show(const ls_opt_t *o,char *out,size_t n)
{
    (void)o;rec_learn_t r;
    if(rec_watch_learn_result(&r))snprintf(out,n,"%lu bd, %u%%",(unsigned long)r.bitrate,r.confidence);
    else snprintf(out,n,"L learns");
}
static const char *o_apply_why(const ls_opt_t *o)
{
    (void)o;rec_learn_t r;
    return !rec_watch_learn_result(&r)?"Nothing learned yet":rec_watch_learn_busy()?"LEARN is measuring":NULL;
}
static void o_recview(const ls_opt_t *o){(void)o;action('v');}
static void o_recview_show(const ls_opt_t *o,char *out,size_t n){(void)o;snprintf(out,n,"level, trigger");}
static const char *o_recview_why(const ls_opt_t *o){(void)o;return can_record()?NULL:"Needs the SDR or the LR2021";}
/* Save to FREQS: what READ is tuned to, as OOK, into mine.csv. */
/* As READ receives it: the LoRa chip's packet receiver is FSK, and so is
   the LR2021 once set to 2-FSK; the rest slice OOK. */
static const char *read_mode(void)
{
    if(rec_watch_source()!=REC_SOURCE_SX1262)return "OOK";
    return ls_lora_chip()!=LS_LORA_CHIP_LR20XX || rec_watch_lr_fsk()?"FSK":"OOK";
}
static void o_freqs(const ls_opt_t *o){(void)o;ls_freqs_offer(rec_get_freq(),read_mode(),NULL,"subghz",feedback,sizeof(feedback));}
static void o_freqs_show(const ls_opt_t *o,char *out,size_t n){(void)o;snprintf(out,n,"%.4f MHz %s",rec_get_freq()/1e6,read_mode());}
static const ls_opt_t OPT_SUBGHZ[]={
    SG_ROW("Frequency",0),SG_ROW("Radio",1),SG_ROW("Capture setup",2),
    {.label="Detect threshold",.kind=LS_OPT_LEVEL,.arg=3,.num=o_gate,.set_num=o_gate_set,
     .lo=REC_SCAN_DETECT_MIN,.hi=REC_SCAN_DETECT_MAX,.step=1,.unit="dB over floor",
     .show=o_row_show,.why_not=o_row_why},
    SG_ROW("On detection",4),SG_ROW("Mesh alerts",5),SG_ROW("Spectrum style",6),
    SG_ROW("Spectrum colour",7),SG_ROW("List legend",8),
    {.label="LR2021 receive",.kind=LS_OPT_CYCLE,.names=LR_RX_NAMES,.n=2,.get=o_lr_rx,
     .set=o_lr_rx_set,.why_not=o_lr_rx_why},
    {.label=LS_TX_UNLOCKED?"TX power (UNLOCKED)":"TX power",.kind=LS_OPT_CYCLE,.names=tx_opt_names,.n=TX_STEPS,.get=o_tx,
     .set=o_tx_set,.why_not=o_tx_why},
    {.label="After a send",.kind=LS_OPT_CYCLE,.names=RESUME_NAMES,.n=2,.get=o_resume,
     .set=o_resume_set},
    /* LEARN applies what it finds by itself, so taking it back - and putting
       it back again - has to be somewhere other than a toast that the next
       key press clears. */
    {.label="Undo LEARN",.kind=LS_OPT_ACTION,.act=o_undo,.show=o_undo_show,.why_not=o_undo_why},
    {.label="Apply last LEARN",.kind=LS_OPT_ACTION,.act=o_apply,.show=o_apply_show,.why_not=o_apply_why},
    /* REC's full level view, which RECORD's banner chip also opens. */
    {.label="Recorder view",.kind=LS_OPT_ACTION,.act=o_recview,.leaves=true,.show=o_recview_show,.why_not=o_recview_why},
    {.label="Save to FREQS",.kind=LS_OPT_ACTION,.act=o_freqs,.show=o_freqs_show},
    /* CATCH's own settings, under its name, filled in at run time: the
       table is const and the CATCH context is another file's. */
    {.label="Catch",.kind=LS_OPT_MENU}};
#undef SG_ROW
#define OPT_ROWS ((int)(sizeof(OPT_SUBGHZ)/sizeof(OPT_SUBGHZ[0])))
#define OPT_UNDO 12
#define OPT_APPLY 13
#define OPT_RECVIEW 14
#define OPT_FREQS 15
#define OPT_CATCH 16
_Static_assert(OPT_CATCH==OPT_ROWS-1 && OPT_ROWS<=32,"the row masks index OPT_SUBGHZ by these numbers");
EXT_RAM_BSS_ATTR static ls_opt_t page_options[OPT_ROWS];
static ls_opt_ctx_t option_ctx={.name="SUB-GHZ",.radio=LS_RSEL_NONE};
static const ls_opt_ctx_t *subghz_options(void)
{
    unsigned mask=0;
    /* While CATCH's bursts are the body, OPTIONS is CATCH's, as it was when
       it had a tab. */
    if(page==PG_BAND && band_catch && ls_catch_options())return ls_catch_options();
    tx_opt_refresh();
    option_ctx.job=LS_RSEL_SUBGHZ_READ;option_ctx.tag=NULL;
    switch(page) {
    case PG_READ:
        mask=(1u<<0)|(1u<<2)|(1u<<5)|(1u<<8)|(1u<<9)|(1u<<10)|(1u<<11)|(1u<<OPT_FREQS);
        if(can_learn())mask|=(1u<<OPT_UNDO)|(1u<<OPT_APPLY);
        if(can_record())mask|=1u<<OPT_RECVIEW;
        break;
    case PG_BAND: mask=(1u<<3)|(1u<<4)|(1u<<6)|(1u<<7)|(1u<<OPT_CATCH);
        option_ctx.job=LS_RSEL_SUBGHZ_SWEEP;option_ctx.tag="SWEEP";break;
    case PG_RECORD: mask=(1u<<0)|(1u<<2);break;
    default: mask=(1u<<OPT_ROWS)-1;break;
    }
    if(!ls_catch_options())mask&=~(1u<<OPT_CATCH);
    option_ctx.opt=page_options;option_ctx.n=0;
    for(int i=0;i<OPT_ROWS;i++)if(mask&(1u<<i)) {
        page_options[option_ctx.n]=OPT_SUBGHZ[i];
        if(i==OPT_CATCH)page_options[option_ctx.n].sub=ls_catch_options();
        option_ctx.n++;
    }
    return &option_ctx;
}
/* ------------------------------------------------------------------ draw */

/* Three places. READ's RECORD view and SAVED's player are reached from
   their place, not from a tab, and light that place's tab. CATCH, which
   needs an RTL-SDR, is part of BAND where there is one. */
#define N_TABS 3
static const char *const TAB_NAME[N_TABS]={"READ","BAND","SAVED"};
static const sg_page_t TAB_PAGE[N_TABS]={PG_READ,PG_BAND,PG_SAVED};
/* Where a tap or a number lands for each page: its place's tab. */
static int tab_of(sg_page_t p) { return p==PG_PLAYER?2:p==PG_RECORD?0:p==PG_BAND?1:p==PG_SAVED?2:0; }

/* REC opens a loaded file in the player, BAND, or the recorder, from
   outside: 1 was CATCH's tab, which is BAND's now. */
void ls_scr_subghz_show(int which)
{
    if(which==0)go(PG_PLAYER);
    else if(which==1)go(PG_BAND);
    else if(which==2)go(PG_RECORD);
    else go(PG_SAVED);
}

static void draw(tui_surface *sf,tui_rect a)
{
    if(a.w<24 || a.h<17) {ls_panel_notice(sf,a,"SUB-GHZ","Enlarge the pane",LS_TEXT(t_scr_subghz_watch_keeps_its_state, "WATCH keeps its state"));return;}
    rec_watch_snapshot(&s);
    static uint32_t scan_generation,learn_generation;
    static bool was_sweeping;
    const bool sweeping=rec_watch_scan_busy();
    if(s.scan_generation!=scan_generation) {
        scan_generation=s.scan_generation;
        if(page==PG_BAND && !listen_pending)snprintf(feedback,sizeof(feedback),"%s",s.scan_result);
    }
    /* LEARN finished: apply what it found, or say it found nothing, and give
       WATCH back. Only for a learn this screen started. */
    if(s.learn_generation!=learn_generation) {
        learn_generation=s.learn_generation;
        if(learn_mine)learn_finish(s.learn_result);
    }
    /* FREQS's hand-off, once the receiver can take it. */
    if(listen_in && !listen_in_since)listen_in_since=esp_timer_get_time();
    if(listen_in && (s.ready || esp_timer_get_time()-listen_in_since>LISTEN_IN_US)) {
        const uint32_t hz=listen_in;listen_in=0;listen_in_since=0;
        bool mode_ok=true;
        if(listen_in_fsk>=0 && rec_watch_source()==REC_SOURCE_SX1262 &&
           ls_lora_chip()==LS_LORA_CHIP_LR20XX && rec_watch_lr_fsk()!=(listen_in_fsk==1)) {
            if(rec_watch_enabled())watch_set(false);
            mode_ok=rec_watch_set_lr_fsk(listen_in_fsk==1);
        }
        /* A WATCH that would not stop keeps the old receiver: listening on it
           would not be the entry's mode. */
        if(!mode_ok)snprintf(feedback,sizeof(feedback),"Stop WATCH to listen in %s",listen_in_fsk==1?"FSK":"OOK");
        else {listen_from="FREQS";listen_here(hz);listen_from="BAND";}
    }
    /* LISTEN HERE, finished: the sweep has let go, so WATCH can have the
       radio - unless the runtime already started it, which it does when
       READ's radio is the one that swept. */
    const bool on_before=s.enabled;
    if(listen_pending && !sweeping) {
        const uint32_t hz=listen_pending;
        listen_pending=0;
        if(!rec_watch_enabled()) {
            if(rec_get_freq()!=hz)rec_set_freq(hz);
            if(!watch_set(true)) {
                const char *why=rec_watch_error();
                snprintf(feedback,sizeof(feedback),"Tuned to %.4f MHz; %s",hz/1e6,
                         why && *why?why:"receiver would not start");
            }
        }
        ls_fresh_bump(&handoff);
    } else if(was_sweeping && !sweeping && !scan_stop_asked &&
              rec_watch_scan_on_hit_get()==REC_SCAN_ON_HIT_CATCH &&
              rec_watch_scan_last_hit() && rec_get_freq()==rec_watch_scan_last_hit()) {
        /* ON DETECTION LISTEN fired: the runtime stopped on the first hit and
           tuned to it. READ follows, as LISTEN HERE would. */
        const uint32_t hz=rec_watch_scan_last_hit();
        if(!rec_watch_enabled())watch_set(true);
        source_frequency[rec_watch_source()]=hz;
        if(page==PG_BAND)go(PG_READ);
        ls_fresh_bump(&handoff);
        snprintf(feedback,sizeof(feedback),"Caught %.4f MHz on the first hit - listening",hz/1e6);
    }
    /* A band, or a zoom, picked mid-sweep, now that the old sweep has let
       go - after the hand-off test above, which a new sweep's start would
       otherwise read as a sweep that stopped by itself. */
    if(scan_next>=0 && !sweeping)scan_start(scan_next);
    else if(zoom_next && !sweeping)zoom_start(zoom_next);
    was_sweeping=sweeping;
    /* What was handed over this frame is in the banner this frame, not the
       next: the snapshot above was taken before WATCH came back. */
    if(rec_watch_enabled()!=on_before)rec_watch_snapshot(&s);
    uint8_t fresh=ls_fresh(&arrivals,s.received,600);
    touch_nav=a.w<90 && a.h>35;
    order_build();
    tx_pump();
    if (!strncmp(feedback,"Sending #",9) && strcmp(s.export_status,"Replay queued") &&
        (!strncmp(s.export_status,"Sent #",6) || !strncmp(s.export_status,"Replay",6)))
        snprintf(feedback,sizeof(feedback),"%s",s.export_status);
    rec_hub_status_t rx;rec_get_hub_status(&rx);
    ls_mixrf_status_t cc;ls_mixrf_snapshot(&cc);
    if(rec_watch_source()==REC_SOURCE_CC1101){rx.freq_hz=rec_get_freq();rx.receiver_streaming=cc.capturing && cc.receiving;}
    /* The SX1262 is not on the RTL's hub. WATCH only turns on once its
       receive session has started, so on this source enabled is listening. */
    if(rec_watch_source()==REC_SOURCE_SX1262){rx.freq_hz=rec_get_freq();rx.receiver_streaming=s.enabled;}
    record_poll(&rx);
    bar_n=0;det_hit=tui_rect_make(0,0,0,0);det_pager=det_hit;det_row_n=0;det_total=0;
    feedback_hit=det_hit;rec_chip_hit=det_hit;
    /* The threshold strip keeps its place while the spectrum is up, so a
       drag can follow the finger; anywhere else it is not on the screen. */
    if(page!=PG_BAND || !sweeping || band_catch)gate_hit=tui_rect_make(0,0,0,0);
    if(page!=PG_BAND)chart_n=0;
    /* The keys for the page in front, LEARN and RECORD only where READ's
       radio has them: a hint for a key that only explains why not is a
       hint for nothing. */
    switch(page) {
    case PG_READ:
        snprintf(hint_text,sizeof(hint_text),"W watch  T send  +/- power  F freq%s%s  O options",
                 can_learn()?"  L learn":"",can_record()?"  D record":"");
        break;
    case PG_BAND:
        snprintf(hint_text,sizeof(hint_text),"N scan  W band  T detect  ENTER listen here%s  O options",
                 LS_LR_EDITION?"":"  X catch");
        break;
    case PG_SAVED:
        snprintf(hint_text,sizeof(hint_text),"T send  +/- power  ENTER open  G files  O options  1-3 pages");
        break;
    case PG_PLAYER:
        snprintf(hint_text,sizeof(hint_text),"P play  +/- power  ESC saved");
        break;
    default:
        snprintf(hint_text,sizeof(hint_text),"D arm/stop  T send last  ESC read");
        break;
    }
    /* The three places, along the bottom where the thumb is - FM's tabs -
       or beside the bar in landscape, on the same rows. */
    if(a.w>a.h*2) {
        tab_w=a.w/4;tabs=tui_rect_make(0,0,0,0);
    } else {
        const int th=ls_tui_compact()?1:a.h>=50?4:3;
        tab_w=0;tabs=tui_rect_make(a.x,a.y+a.h-th,a.w,th);
        a.h-=th;
    }
    bar_rx_now=rx;bar_rx=&bar_rx_now;bar_fresh=fresh;
    page_body=a;
    switch(page) {
    case PG_READ:     draw_read(sf,a,&rx,&cc,fresh); break;
    case PG_BAND:     draw_band(sf,a); break;
    case PG_SAVED:    draw_saved(sf,a); break;
    case PG_PLAYER:   draw_player(sf,a); break;
    case PG_RECORD:   draw_record(sf,a); break;
    }
    /* A page change sweeps a shaded column across the new body: the wipe
       FM draws over its dial when a mode change rebuilds the receiver, here
       behind the page that replaced the last one, so the change is seen
       whichever way it came - a tab, a key, LISTEN HERE - without a word of
       the new page being covered while it reads. */
    if(page_body.w>4 && page_body.h>2)
        ls_radio_glass_wipe_behind(&glass,sf,page_body,page_body.y+1,page_body.y+page_body.h-1,(int)page);
    ls_btn_t tab[N_TABS];
    for(int i=0;i<N_TABS;i++)
        tab[i]=(ls_btn_t){TAB_NAME[i],NULL,(char)('1'+i),tab_of(page)==i,false,false};
    /* A place is lit while it has the radio, as the old tabs were. */
    tab[0].on=tab[0].on || s.enabled || tx_paused_watch || rec_watch_learn_busy() || rec_mine;
    tab[1].on=tab[1].on || sweeping || ls_catch_running();
    tab[2].badge=saved_new;
    if(tabs.h>0)ls_btn_bar_rows_slot(sf,tabs,tab,N_TABS,-1,LS_BTN_SLOT_QUICK,1);
}

/* ------------------------------------------------------------ keys, taps */

static bool back_key(ls_tk_t k,char ch)
{
    return k==LS_TK_ESC || k==LS_TK_BACKSPACE || (k==LS_TK_CHAR && (ch=='b' || ch=='B'));
}

/* ENTER on BAND: the picked detection, else the loudest column; on CATCH's
   bursts, the picked burst, else CATCH itself. Whatever it opens has LISTEN
   HERE first. */
static void band_enter(void)
{
#if !LS_LR_EDITION
    if(band_catch) {
        const uint32_t hz=ls_catch_selected_hz();
        if(hz)band_menu_open(BAND_BURST,hz,NULL);
        else action('x');
        return;
    }
#endif
    static EXT_RAM_BSS_ATTR rec_scan_bin_t hit[REC_SCAN_BINS];
    const int n=hits_sorted(hit);
    if(n) {
        if(det_sel>=n)det_sel=n-1;
        if(det_sel<0)det_sel=0;
        detection_open(hit[det_sel].hz);
    } else if(chart_n)peak_open(peak_column());
}

static bool key(ls_tk_t k,char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    /* 1, 2 and 3 are the three places, from anywhere in them. */
    if(k==LS_TK_CHAR && ch>='1' && ch<'1'+N_TABS) {
        go(TAB_PAGE[ch-'1']);
        return true;
    }
    if(back_key(k,ch)) {
        if(page==PG_PLAYER){go(PG_SAVED);return true;}
        if(page!=PG_READ){go(PG_READ);return true;}
        return false;
    }
    /* The player owns ENTER, LEFT/RIGHT and +/- : it is the page. */
    if(page==PG_PLAYER && k!=LS_TK_TAB) {
        const int r=ls_rec_player_key(k,ch);
        if(r==2){go(PG_SAVED);return true;}
        if(r==1)return true;
    }
    /* A focused button on the bar first, so ENTER on BACK goes back. LEFT
       and RIGHT walk the bar, except in the player, where they walk the
       pulses. */
    if((k==LS_TK_TAB ||
       (page!=PG_PLAYER && (k==LS_TK_LEFT || k==LS_TK_RIGHT))))
        return ls_btn_navigate(k,&button_slot,&button_focus,false);
    if(k==LS_TK_ENTER && button_focus>=0 && button_focus<bar_n) {
        action(bar_keys[button_focus]);
        return true;
    }
    /* Then the page's own keys, before the shortcuts every page shares.
       RECORD's ENTER is ARM, through RECORD so the banner knows. */
    if(page==PG_RECORD && k==LS_TK_ENTER){action('d');return true;}
    if(k==LS_TK_UP || k==LS_TK_DOWN)button_focus=-1;
    switch(page) {
    case PG_READ:
        if(k==LS_TK_UP){pick_move(-1);return true;}
        if(k==LS_TK_DOWN){pick_move(1);return true;}
        if(k==LS_TK_ENTER){action('c');return true;}
        break;
    case PG_SAVED:
        if(k==LS_TK_UP){saved_free=false;if(saved_sel)saved_sel--;return true;}
        if(k==LS_TK_DOWN){saved_free=false;if(saved_sel+1<saved_total())saved_sel++;return true;}
        if(k==LS_TK_ENTER){action('O');return true;}
        break;
    case PG_BAND:
        if(band_catch && (k==LS_TK_UP || k==LS_TK_DOWN))return ls_catch_key(k,0);
        if(k==LS_TK_UP || k==LS_TK_DOWN) {
            static EXT_RAM_BSS_ATTR rec_scan_bin_t hit[REC_SCAN_BINS];
            const int n=rec_watch_scan_result(hit,REC_SCAN_BINS);
            if(n>0) {
                /* Where the list is behind VIEW, walking it brings it up. */
                sweep_hits=true;det_free=false;
                if(k==LS_TK_UP && det_sel>0)det_sel--;
                if(k==LS_TK_DOWN && det_sel+1<n)det_sel++;
                if(det_sel>=n)det_sel=n-1;
            }
            return true;
        }
        if(k==LS_TK_ENTER){band_enter();return true;}
        break;
    default: break;
    }
    if(k!=LS_TK_CHAR || !ch)return false;
    /* The letter printed on a button of this page's bar wins over the shared
       shortcut with the same letter - on BAND, W is BAND PICK, not WATCH. */
    /* An exact letter first, so o is OPTIONS even beside OPEN's O. */
    for(int i=0;i<bar_n;i++)
        if(bar_keys[i]==ch){action(bar_keys[i]);return true;}
    for(int i=0;i<bar_n;i++) {
        char b=bar_keys[i];
        if(b>='A' && b<='Z')b+='a'-'A';
        if(b==ch){action(bar_keys[i]);return true;}
    }
    if(page==PG_READ) {
        if(ch==','){pick_move(-1);return true;}
        if(ch=='.'){pick_move(1);return true;}
    }
    if(page!=PG_BAND && (ch=='+' || ch=='=' || ch=='-')) {
        action(ch=='-'?'-':'+');
        return true;
    }
    /* Every action keeps its letter on every page - a shortcut that stopped
       working because its button moved would be a worse trade than the
       crowding it fixed. L learns and D records from anywhere, and take the
       screen to READ, where their banner is; Z undoes a LEARN; V is RECORD's
       full view; X is CATCH, on BAND. R stays RADIO, as in every app - REC
       answers R itself before this screen sees it. */
    if(!strchr("fswncmpejadirlgtzv",ch) && !(ch=='x' && !LS_LR_EDITION))return false;
    action(ch);return true;
}

static bool touch(int x,int y)
{
    if(tabs.h>0 && tui_rect_contains(tabs,x,y)) {
        const int t=ls_btn_hit_slot(x,y,LS_BTN_SLOT_QUICK);
        if(t>=0 && t<N_TABS)go(TAB_PAGE[t]);
        return true;
    }
    {
        const int i=ls_btn_hit(x,y);
        if(i>=0 && i<bar_n){action(bar_keys[i]);return true;}
    }
    /* The banner's chips, on every page that has the banner: the detections
       counter leads to BAND, RECORD's chip to its full view. */
    if(rec_chip_hit.w>0 && tui_rect_contains(rec_chip_hit,x,y)){go(PG_RECORD);return true;}
    if(page!=PG_BAND && det_hit.w>0 && tui_rect_contains(det_hit,x,y)){go(PG_BAND);return true;}
    switch(page) {
    case PG_PLAYER: {
        const int r=ls_rec_player_touch(x,y);
        if(r==2)go(PG_SAVED);
        return true;
    }
    case PG_RECORD:
        /* ARM goes through RECORD, so the banner on READ knows. */
        if(can_record() && ls_rec_tools_button_hit(x,y)){action('d');return true;}
        if(can_record())return ls_rec_tools_touch(x,y);
        return true;
    case PG_SAVED:
        if(tui_rect_contains(saved_list,x,y)) {
            if(ls_drag_scroll_tap_is_lift(&saved_drag))return true;   /* the end of a drag, not a choice */
            const int i=saved_hit(y);
            if(i>=0){
                saved_free=false;
                /* First tap selects, a tap on the selected entry opens it. */
                if(i==saved_sel)action('O');
                else saved_sel=i;
            }
        }
        return true;
    case PG_BAND:
#if !LS_LR_EDITION
        if(band_catch) {
            /* First tap picks a burst, a tap on the picked one opens it. */
            const int before=ls_catch_selected_index();
            const bool on_list=ls_catch_list_contains(x,y);
            ls_catch_body_touch(x,y);
            if(on_list && before>=0 && before==ls_catch_selected_index())band_enter();
            return true;
        }
#endif
        if(det_pager.w>0 && det_total>det_row_n && tui_rect_contains(det_pager,x,y)) {
            det_free=false;
            if(x<det_pager.x+det_pager.w/2)det_sel=det_first>0?det_first-1:0;
            else det_sel=det_first+det_row_n<det_total?det_first+det_row_n:det_total-1;
            return true;
        }
        if(det_hit.w>0 && tui_rect_contains(det_hit,x,y)) {
            sweep_hits=!sweep_hits;feedback[0]=0;return true;
        }
        if(det_row_n && tui_rect_contains(det_rows,x,y)) {
            if(ls_drag_scroll_tap_is_lift(&det_drag))return true;   /* the end of a drag, not a choice */
            const int i=y-det_rows.y;
            if(i>=0 && i<det_row_n){det_sel=det_first+i;det_free=false;detection_open(det_row_hz[i]);}
            return true;
        }
        /* A tap on the gate's track puts the gate at that height: a quick
           tap can be over before a frame sees the finger held, and the
           track is the control for exactly this. Drags follow in
           gate_follow_drag. */
        if(gate_hit.h>0 && tui_rect_contains(gate_hit,x,y)) {
            rec_watch_scan_set_threshold(gate_db_at_row(y));
            LS_SNPRINTF(t_scr_subghz_detect_above_0f_db, feedback, sizeof(feedback), "Detect above %.0f dB over the floor",
                     (double)rec_watch_scan_threshold());
            return true;
        }
        /* Anywhere else on the chart, the column under the finger. */
        if(chart_n && tui_rect_contains(chart_hit,x,y)){peak_open(chart_column(x));return true;}
        return true;
    case PG_READ: {
        if(feedback_hit.w>0 && tui_rect_contains(feedback_hit,x,y)){action('z');return true;}
        const int i=ls_btn_hit_slot(x,y,LS_BTN_SLOT_WATERFALL);
        if(i==0){pick_move(-1);return true;}
        if(i==1){pick_move(1);return true;}
        if(!notes && pulse_total && tui_rect_contains(pulse_hit,x,y)) {
            uint64_t t=(uint64_t)(x-pulse_hit.x)*pulse_total/pulse_hit.w,sum=0;
            int n=s.event[selected].edges;if(n>48)n=48;
            for(int j=0;j<n;j++) {sum+=s.preview[selected][j]<0?-(int64_t)s.preview[selected][j]:s.preview[selected][j];if(sum>t){pulse_selected=j;break;}}
            return true;
        }
        if(x>=list.x && x<list.x+list.w && y>=list.y && y<list.y+list.h) {
            if(ls_drag_scroll_tap_is_lift(&read_drag))return true;   /* the end of a drag, not a choice */
            const int n=read_first+(y-list.y)/2;
            /* First tap selects, a tap on the selected capture opens it. */
            if(n<ord_n){if(n==pick_pos)action('c');else pick_at(n);}
        }
        return true;
    }
    default: return true;
    }
}
const ls_tui_screen_t ls_scr_subghz={.radio="REC",.name="SUB-GHZ",.hint=hint_text,.enter=enter,.leave=leave,.draw=draw,.key=key,.touch=touch};
