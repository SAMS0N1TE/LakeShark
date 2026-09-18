#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_numpad.h"
#include "../../ls_picker.h"
#include "../../ls_motion.h"
#include "../../ls_field.h"
#include "rec_state.h"
#include "rec_watch.h"
#include "ls_mesh.h"
#include "ls_mixrf.h"
#include "../../ls_waterfall.h"
#include "settings.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int button_focus=-1, button_slot;
/* The top bar's shortcut characters, in the order the bar draws them. The
   dispatchers key off the character, so this is the only thing that has to
   agree with the bar - move a button, move a letter. */
/* Five in the bar, and two menus behind it.

   Ten buttons in one row is a pile, not a flow: everything was equally
   prominent, nothing said what to do first, and every new action made it
   worse. What is left on the bar is the session - point the receiver, set it
   up, start it - plus one way into the things you do to a capture. The rest
   moved into pickers, which this app already uses for BANDS and SETUP, so
   nothing new had to be invented and the next action goes in a list rather
   than on the bar. */
/* The top bar, in the order the job is done. BANDS left it when TUNE grew a
   list - a preset frequency and a typed one are the same question - and SCAN
   took the slot, because a sweep that can only be stopped from inside a menu
   is a sweep you cannot stop in a hurry. */
static const char BTN_KEYS[]="fswnc";
/* Defined below, beside the other source helpers, but the banner draws
   above them. */
static bool source_takes_the_mesh(void);
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
static float gate_hit_span;
static int pulse_selected;
static uint64_t pulse_total;
/* Two flags, because DETAILS was one and meant two unrelated things: while a
   sweep ran it chose detections over the spectrum, and the rest of the time
   it laid a legend over the capture list. Sharing a bool meant turning the
   legend off changed what the next sweep would draw, which nothing on the
   screen explained. */
static bool sweep_hits;   /* sweep pane: detections instead of the spectrum */
static bool notes;        /* capture list: the legend overlay              */
static ls_fresh_t arrivals;
static int setup_item;
/* Sized off the enum, not off how many sources there happened to be when
   this was written. */
static uint32_t source_frequency[REC_SOURCE_COUNT]={433920000,433920000,433920000};
extern void ls_scr_rec_tools(void);
static const char *source_name(void) { return rec_source_name(rec_watch_source()); }

/* When WATCH last started, so the banner can show a session running rather
   than a flag that is merely set. Reset on every transition, which is also
   what makes a restart visible. */
static int64_t listening_since;

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
    const bool live = s->enabled && rx->receiver_streaming;
    const bool trouble = s->enabled && !rx->receiver_streaming;

    const char *word = !s->ready ? "LOADING"
                     : !s->enabled ? "STOPPED"
                     : live ? "LISTENING"
                     : "NO RECEIVER";
    const uint8_t hue = !s->enabled ? TUI_CYAN
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
       receiver reads as frozen instead of as idle. */
    snprintf(line, sizeof(line), " %c %s ", ls_motion_pip(live), word);
    tui_put_str(sf, r, r.x, r.y, line, TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT));

    snprintf(line, sizeof(line), "%.4f MHz", rx->freq_hz / 1e6);
    if ((int)strlen(line) + 4 < r.w)
        tui_put_str(sf, r, r.x + r.w - 1 - (int)strlen(line), r.y, line,
                    TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

    /* Second line: elapsed, what has been heard, and whether it is on the
       card. A session that has been up for four minutes with nothing heard is
       a different situation from one that just started, and the old screen
       showed them identically. */
    if (r.h < 2) return;
    if (source_takes_the_mesh() && s->enabled && r.w > 40) {
        static const char *const M = "MESH PAUSED";
        tui_put_str(sf, r, r.x + (int)strlen(word) + 6, r.y, M,
                    TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }
    const uint32_t up = (s->enabled && listening_since)
                      ? (uint32_t)((now - listening_since) / 1000000) : 0;
    snprintf(line, sizeof(line), "%lu:%02lu   %d capture%s   heard %lu",
             (unsigned long)(up / 60), (unsigned long)(up % 60),
             s->count, s->count == 1 ? "" : "s",
             (unsigned long)s->received);
    tui_put_str(sf, r, r.x + 1, r.y + 1, line,
                ls_fresh_attr(fresh, TUI_WHITE | TUI_BRIGHT,
                              TUI_GREEN | TUI_BRIGHT, TUI_BLACK));

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
    if (rec_watch_source() != REC_SOURCE_CC1101) return rec_source_name(rec_watch_source());
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
        snprintf(feedback,sizeof(feedback),"CC1101: 300-348 / 387-464 / 779-928 MHz");return;
    }
    if(!isfinite(mhz) || mhz<24 || mhz>1766) {snprintf(feedback,sizeof(feedback),"Enter 24-1766 MHz; receiver must support it");return;}
    if(rec_watch_enabled()) {snprintf(feedback,sizeof(feedback),"Stop WATCH before changing frequency");return;}
    rec_disarm();rec_set_freq((uint32_t)(mhz*1e6+.5));
}
static void peer_done(int i)
{
    if(i==0)rec_watch_alert_target("");
    else if(i<=LS_MESH_MAX_PEERS && peers[i-1][0])rec_watch_alert_target(peers[i-1]);
}
static void setup_value(double value)
{
    static const double low[]={0,2,4,10,6},high[]={255,2000,10000,30000,4096};
    if(rec_watch_enabled()) {snprintf(feedback,sizeof(feedback),"Stop WATCH before changing capture setup");return;}
    if(setup_item<0 || setup_item>4 || !isfinite(value) || value<low[setup_item] || value>high[setup_item]) {
        snprintf(feedback,sizeof(feedback),"Value outside the displayed range");return;
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
        snprintf(feedback,sizeof(feedback),"Value outside the displayed range");return;
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
        snprintf(feedback,sizeof(feedback),"Refused - check the rate and deviation");
        return;
    }
    rec_watch_fsk_get(&m);
    snprintf(feedback,sizeof(feedback),"%lu bd  %.1f kHz dev  %u kHz bw  sync %08lX",
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
    ls_picker_open("SX1262 RECEIVE",fsk_done);
    ls_picker_add("Bit rate",d0);
    ls_picker_add("Deviation",d1);
    ls_picker_add("Bandwidth",d2);
    ls_picker_add("Preamble",d3);
    ls_picker_add("Sync word",d4);
    ls_picker_add("Restore defaults","4800 / 25k / 2DD42DD4");
}
static void setup_done(int i)
{
    if(rec_watch_enabled()) {snprintf(feedback,sizeof(feedback),"Stop WATCH before changing capture setup");return;}
    if(i==5) {
        rec_disarm();rec_set_thresh(0);rec_set_gap_ms(30);rec_set_min_pulse(40);
        rec_set_max_span(8000000);rec_set_min_edges(6);
        snprintf(feedback,sizeof(feedback),"Capture defaults restored");return;
    }
    if(i<0 || i>4)return;
    setup_item=i;
    const char *title[]={"THRESHOLD 0=AUTO,1-255","END GAP 2-2000 ms","MIN PULSE 4-10000 us","MAX SPAN 10-30000 ms","MIN EDGES 6-4096"};
    const char *unit[]={"magnitude","ms","us","ms","edges"};
    double value[]={rec_get_thresh(),rec_get_gap_ms(),rec_get_min_pulse(),rec_get_max_span()/1000.,rec_get_min_edges()};
    ls_numpad_open(title[i],unit[i],value[i],setup_value);
}
/* WHY a control is greyed out, in the words of the thing blocking it.

   A disabled button that says nothing when pressed teaches the operator that
   the screen is broken. Every one of these is a real precondition somewhere
   below; saying it out loud costs a line and removes the guessing. NULL means
   the control is live. */
static const char *disabled_reason(char c)
{
    const bool watching=rec_watch_enabled();
    switch(c) {
    case 'w': return s.ready?NULL:"Still loading capture history from the card";
    case 'f': return watching?"Stop WATCH before retuning":NULL;
    /* Stopping a sweep must never be refused, and a sweep cannot be running
       while WATCH is, so the guard is only about starting one. */
    case 'n': return rec_watch_scan_busy()?NULL:
                     watching?"Stop WATCH before sweeping":NULL;
    case 's': return watching?"Stop WATCH before changing capture settings":
                     rec_watch_source()==REC_SOURCE_CC1101?
                     "CC1101 capture settings are fixed":
                     NULL;
    case 'r': return watching?"Stop WATCH before changing source":NULL;
    case 'p': return s.count?NULL:"Nothing captured yet";
    case 'j': return s.count?NULL:"Nothing captured yet";
    case 'e': return !s.count?"Nothing captured yet":
                     s.exporting?"An export is already running":NULL;
    case 'd': return rec_watch_source()!=REC_SOURCE_RTL?
                     "Receiver tools belong to the RTL source":
                     watching?"Stop WATCH to open receiver tools":NULL;
    default:  return NULL;
    }
}

/* The menus call straight into it, and it is defined below them. */
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
   leaving the operator to know what a dBm is. */
static const int REPLAY_DBM[] = {-9, 0, 14, 22};
static void replay_power_done(int index)
{
    if (index < 0 || index >= (int)(sizeof(REPLAY_DBM)/sizeof(REPLAY_DBM[0]))) return;
    rec_watch_snapshot(&s);
    if (selected >= s.count) return;
    const rec_watch_event_t *e = &s.event[selected];
    if (!rec_watch_request_replay(e->id, REPLAY_DBM[index]))
        snprintf(feedback,sizeof(feedback),"Stop WATCH before replaying");
    else
        snprintf(feedback,sizeof(feedback),"Sending #%lu at %d dBm...",
                 (unsigned long)e->id, REPLAY_DBM[index]);
}
static void replay_power_open(void)
{
    static const char *const LABEL[] = {"-9 dBm","0 dBm","14 dBm","22 dBm"};
    static const char *const WHY[] = {
        "Bench test, a few feet",
        "Same room",
        "Normal range",
        "Maximum - check your licence"
    };
    ls_picker_open("SEND AT", replay_power_done);
    for (int i = 0; i < (int)(sizeof(REPLAY_DBM)/sizeof(REPLAY_DBM[0])); i++)
        ls_picker_add(LABEL[i], WHY[i]);
}

static void capture_menu_done(int index)
{
    if (index < 0 || index >= menu_n) return;
    rec_watch_snapshot(&s);
    if (selected >= s.count) return;
    const rec_watch_event_t *e = &s.event[selected];
    switch (menu_id[index]) {
    case 'R': replay_power_open(); break;
    case 'S':
        if (!rec_watch_request_export(e->id))
            snprintf(feedback,sizeof(feedback),"Export busy or capture unavailable");
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
static int scan_grain=-1;    /* 0 shade, 1 ascii, 2 bars               */
/* How many dB above the floor the top of the display means. Wide enough
   that noise stays down in the grass and a signal has somewhere to go. */
#define SCAN_WINDOW_DB 45.0f
#define SCAN_LOOKS (1 + LS_WF_PAL__COUNT)
#define SCAN_GRAINS 3
static const char *const SCAN_LOOK_NAME[SCAN_LOOKS] =
    {"dB BANDS","HEAT","ICE","PHOSPHOR","NEON"};
static const char *const SCAN_GRAIN_NAME[SCAN_GRAINS] =
    {"SHADE","ASCII","BARS"};

static void scan_look_load(void)
{
    if(scan_look<0) scan_look=settings_get_subghz_colour();
    if(scan_grain<0) scan_grain=settings_get_subghz_style();
    if(scan_look>=SCAN_LOOKS) scan_look=0;
    if(scan_grain>=SCAN_GRAINS) scan_grain=0;
}

/* Colour IS the reading, not decoration: each band is a real number of dB
   above the measured noise floor, and the legend on screen says which. A
   palette chosen to look busy would make a quiet band look like a find. */
static uint8_t scan_hue(float over_floor)
{
    if (over_floor >= 40.0f) return TUI_RED;       /* very close, or strong */
    if (over_floor >= 25.0f) return TUI_YELLOW;    /* comfortably readable  */
    if (over_floor >= REC_SCAN_DETECT_DB) return TUI_GREEN;   /* a detection */
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
    switch (scan_grain) {
    case 1: {
        static const char RAMP[] = " .:-=+*#%@";
        int i = level * 10 / 16;
        if (i < 0) i = 0;
        if (i > 9) i = 9;
        return RAMP[i];
    }
    case 2:
        if (eighths > 0) return LS_TUI_TRACE(eighths > 8 ? 8 : eighths);
        return LS_TUI_TRACE(8);
    default:
        return level >= 12 ? LS_TUI_SHADE_75
             : level >= 6  ? LS_TUI_SHADE_50 : LS_TUI_SHADE_25;
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
    const int hits = rec_watch_scan_hits();
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

/* WHAT IT ACTUALLY FOUND, as a table rather than a picker.

   The old answer was a list of frequencies, which cannot distinguish a
   transmitter that spoke once from one that is still going. Count and
   age are what make a detection worth acting on. */
static void hits_table(tui_surface *sf, tui_rect r)
{
    static EXT_RAM_BSS_ATTR rec_scan_bin_t hit[REC_SCAN_BINS];
    const int n = rec_watch_scan_result(hit, REC_SCAN_BINS);
    const float floor_dbm = rec_watch_scan_live_floor();
    char line[110];

    snprintf(line, sizeof(line), "DETECTIONS  %d over %.0f dB", n,
             (double)rec_watch_scan_threshold());
    tui_put_str(sf, r, r.x, r.y, line,
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    if (!n) {
        tui_put_str(sf, r, r.x, r.y + 2,
                    "Nothing has crossed the threshold yet.", LS_ATTR_DIM);
        tui_put_str(sf, r, r.x, r.y + 3,
                    "Leave SCAN running and transmit.", LS_ATTR_DIM);
        return;
    }
    snprintf(line, sizeof(line), "%-11s %7s %5s %5s %8s",
             "MHz", "dBm", "over", "seen", "last");
    tui_put_str(sf, r, r.x, r.y + 1, line, LS_ATTR_DIM);

    const int64_t now = esp_timer_get_time();
    const int rows = r.h - 2;
    for (int i = 0; i < n && i < rows; i++) {
        const float over = hit[i].dbm - floor_dbm;
        char age[12];
        if (!hit[i].last_us) snprintf(age, sizeof(age), "--");
        else {
            const uint32_t s_ago = (uint32_t)((now - hit[i].last_us) / 1000000);
            if (s_ago < 60) snprintf(age, sizeof(age), "%lus", (unsigned long)s_ago);
            else snprintf(age, sizeof(age), "%lum", (unsigned long)(s_ago / 60));
        }
        snprintf(line, sizeof(line), "%-11.4f %7.0f %5.0f %5lu %8s",
                 hit[i].hz / 1e6, (double)hit[i].dbm, (double)over,
                 (unsigned long)hit[i].seen, age);
        /* The strongest is what a glance should land on. */
        tui_put_str(sf, r, r.x, r.y + 2 + i, line,
                    i == 0 ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
                           : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    }
    if (n > rows)
        tui_put_str(sf, r, r.x, r.y + r.h - 1, "...more in SCAN RESULTS",
                    LS_ATTR_DIM);
}

static void spectrum(tui_surface *sf, tui_rect r)
{
    scan_look_load();
    static EXT_RAM_BSS_ATTR rec_scan_bin_t bins[REC_SCAN_BINS];
    const int n = rec_watch_scan_live(bins, REC_SCAN_BINS);
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
    float top = floor_dbm + SCAN_WINDOW_DB;
    if (bins[peak_i].dbm > top) top = bins[peak_i].dbm;

    /* Header: what it is doing, and the loudest thing it can hear. */
    char line[96];
    if (hit_flashing()) {
        /* The whole header row, so it cannot be mistaken for the usual
           status text, and inverted on the blink. */
        snprintf(line, sizeof(line), " HIT  %.4f MHz ", hit_flash_hz / 1e6);
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
    const int gate_row = rows - 1 - (int)(gate_db / span * rows + 0.5f);
    {
        if (gate_row > 0 && gate_row < rows)
            for (int x = 0; x < r.w; x += 2)
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
    if (r.w >= 20 && rows >= 4) {
        const int gw = 3;
        const int gx = r.x + r.w - gw;
        gate_hit = tui_rect_make(gx, top_row, gw, rows);
        gate_hit_span = span;
        for (int y = 0; y < rows; y++) {
            const bool at = (y == gate_row);
            for (int k = 0; k < gw; k++)
                tui_put_char(sf, r, gx + k, top_row + y,
                             at ? LS_TUI_SHADE_75 : LS_TUI_SHADE_25,
                             at ? TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK)
                                : TUI_ATTR(TUI_RED, TUI_BLACK));
        }
        /* The number rides with the marker, one row clear of the track ends
           so it is never half off the pane. */
        char gt[8];
        snprintf(gt, sizeof(gt), "%2.0f", (double)gate_db);
        int gy = gate_row;
        if (gy < 0) gy = 0;
        if (gy > rows - 1) gy = rows - 1;
        tui_put_str(sf, r, gx, top_row + gy, gt,
                    TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT));
    } else {
        gate_hit = tui_rect_make(0, 0, 0, 0);
    }

    for (int i = 0; i < n; i++) {
        const int xa = r.x + (int)((long)i * r.w / n);
        const int xb = r.x + (int)((long)(i + 1) * r.w / n);
        if (xb <= xa) continue;

        const float over = bins[i].dbm - floor_dbm;
        float v = over / span;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        /* In eighths of a cell, so the BARS grain can draw a partial top. */
        const int he = (int)(v * eighth_rows + 0.5f);
        const int h = (he + 7) / 8;

        float hv = (bins[i].hold - floor_dbm) / span;
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
                g = LS_TUI_SHADE_25;
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
        const int px = r.x + (int)((long)peak_i * r.w / n) + (r.w / n) / 2;
        float pv = (bins[peak_i].dbm - floor_dbm) / span;
        if (pv < 0) pv = 0;
        if (pv > 1) pv = 1;
        const int ph = (int)(pv * rows + 0.5f);
        /* On a hit the marker blinks with the header, so the eye is led from
           one to the other rather than having to find the column itself. */
        const bool blink = hit_flash_on();
        for (int y = 0; y < rows - ph; y++)
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
    if (hits) snprintf(line, sizeof(line), "%d found  floor %.0f dBm",
                       hits, (double)floor_dbm);
    else      snprintf(line, sizeof(line), "nothing yet  floor %.0f dBm",
                       (double)floor_dbm);
    if ((int)strlen(line) + 2 < r.w)
        tui_put_str(sf, r, r.x + r.w - 1 - (int)strlen(line), r.y + r.h - 2,
                    line, hits ? TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
                               : LS_ATTR_DIM);

    /* What the colours mean, in dB over that floor. Only for the banded
       look: a continuous palette is a ramp, not five named steps, and
       labelling it with five would be a lie about what it shows. */
    if (!scan_look && r.h >= 8 && r.w >= 44) {
        static const struct { const char *t; float d; } KEY[] = {
            {"noise", 0}, {"+4", 4}, {"found", REC_SCAN_DETECT_DB},
            {"+25", 25}, {"+40", 40}
        };
        int x = r.x;
        for (unsigned i = 0; i < sizeof(KEY)/sizeof(KEY[0]); i++) {
            const int w = (int)strlen(KEY[i].t) + 2;
            if (x + w >= r.x + r.w) break;
            tui_put_char(sf, r, x, r.y + r.h - 2, LS_TUI_SHADE_75,
                         TUI_ATTR(scan_hue(KEY[i].d) | TUI_BRIGHT, TUI_BLACK));
            tui_put_str(sf, r, x + 1, r.y + r.h - 2, KEY[i].t, LS_ATTR_DIM);
            x += w;
        }
    }
}

static void scan_result_done(int index)
{
    rec_scan_bin_t bins[REC_SCAN_BINS];
    const int n=rec_watch_scan_result(bins,REC_SCAN_BINS);
    if(index<0 || index>=n)return;
    if(rec_watch_enabled()){snprintf(feedback,sizeof(feedback),"Stop WATCH first");return;}
    source_frequency[rec_watch_source()]=bins[index].hz;
    rec_set_freq(bins[index].hz);
    snprintf(feedback,sizeof(feedback),"Tuned to %.4f MHz (%.0f dBm)",
             bins[index].hz/1e6,(double)bins[index].dbm);
}
static void scan_results_open(void)
{
    rec_scan_bin_t bins[REC_SCAN_BINS];
    const int n=rec_watch_scan_result(bins,REC_SCAN_BINS);
    const float floor_dbm=rec_watch_scan_floor();
    ls_picker_open("SCAN RESULTS",scan_result_done);
    ls_picker_empty_reason("No sweep yet - run SCAN first");
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
static void scan_range_done(int index)
{
    /* `bins` is the capture probability as much as the resolution - see
       rec_watch_request_scan. A wide sweep is for finding something and can
       afford to miss bursts; the narrow one is for watching a transmitter
       already found, and must not. */
    static const struct { uint32_t lo,hi; int bins; } R[]={
        {0,0,8},                        /* watch: nearly every burst lands  */
        {0,1,32},                       /* hunt around here                 */
        {433050000u,434790000u,32},
        {862000000u,870000000u,32},
        {902000000u,928000000u,32},
        {150000000u,170000000u,32}
    };
    if(index<0 || index>=(int)(sizeof(R)/sizeof(R[0])))return;
    uint32_t lo=R[index].lo,hi=R[index].hi;
    const int bins=R[index].bins;
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
    /* WATCH is the usual reason and the only one the operator can act on,
       but it is not the only one - the receiver can simply refuse. Saying
       "stop WATCH" to somebody who is not watching sends them looking for a
       button that is already off. */
    if(!rec_watch_request_scan(lo,hi,0,bins))
        snprintf(feedback,sizeof(feedback),"%s",
                 rec_watch_enabled()?"Stop WATCH before scanning":
                                     "The receiver would not start a sweep");
    else
        snprintf(feedback,sizeof(feedback),"Sweeping %.3f-%.3f MHz, %d bins - SCAN again to stop",
                 lo/1e6,hi/1e6,bins);
}
static void scan_open(void)
{
    ls_picker_open("SCAN WHERE",scan_range_done);
    ls_picker_add("Watch this frequency","+/-150 kHz, every burst");
    ls_picker_add("Hunt around here","+/- 1 MHz");
    ls_picker_add("433 MHz","433.05 - 434.79");
    ls_picker_add("868 MHz","862 - 870");
    ls_picker_add("915 MHz","902 - 928");
    ls_picker_add("VHF","150 - 170");
}

/* LEARN and WHAT IT HEARD were two rows of the same menu, and the second was
   empty until the first had been run - so the operator met a row that said
   "Review and apply" and did nothing, with no way to tell from the list that
   the order mattered. One list now: the measurement at the top, and what it
   found underneath once there is anything to show.

   The facts are rows rather than a paragraph because a number offered
   without its confidence gets treated as fact. */
static void learn_open(void)
{
    /* Thirty seconds: a quarter measuring the tone separation, the rest
       trying seven bit rates. Every one of them needs the transmitter to be
       transmitting, so the instruction matters as much as the button. */
    if(!rec_watch_request_learn(30))
        snprintf(feedback,sizeof(feedback),"Stop WATCH before learning");
    else
        snprintf(feedback,sizeof(feedback),"Learning for 30s - keep pressing the remote");
}
static void learn_menu_done(int index)
{
    if(index<0 || index>=menu_n)return;
    rec_learn_t r;
    switch(menu_id[index]) {
    case 'R': learn_open(); break;
    case 'A':
        if(!rec_watch_learn_result(&r))return;
        if(!rec_watch_learn_apply()){snprintf(feedback,sizeof(feedback),"Stop WATCH first");return;}
        snprintf(feedback,sizeof(feedback),"Applied %lu baud, sync %08lX",
                 (unsigned long)r.bitrate,(unsigned long)r.sync_word);
        break;
    default: break;   /* the fact rows are readings, not controls */
    }
}
static void learn_signal_open(void)
{
    rec_learn_t r;
    const bool have=rec_watch_learn_result(&r);
    menu_n=0;
    ls_picker_open("LEARN SIGNAL",learn_menu_done);
    menu_add('R',have?"LISTEN AGAIN":"LISTEN 30s","Keep the transmitter going");
    if(!have)return;
    /* d is wider than the rest because it carries r.note, which is 56 bytes
       and is the whole point of the row: at 28 the percentage survived and
       the reason for it - the part worth reading - was cut off mid-word. */
    char a[28],b[28],c[28],d[64];
    snprintf(a,sizeof(a),"%lu baud",(unsigned long)r.bitrate);
    snprintf(b,sizeof(b),"%.1f kHz",r.deviation_hz/1000.0);
    snprintf(c,sizeof(c),"%08lX",(unsigned long)r.sync_word);
    snprintf(d,sizeof(d),"%u%% - %s",r.confidence,r.note);
    menu_add('A',"APPLY THESE",a);
    menu_add('.',"Deviation",b);
    menu_add('.',"Sync word",c);
    menu_add('.',"Confidence",d);
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
    settings_set_subghz_style(index);
    snprintf(feedback, sizeof(feedback), "Style: %s", SCAN_GRAIN_NAME[index]);
}
static void style_open(void)
{
    static const char *const WHY[SCAN_GRAINS] = {
        "Blocks, densest fill",
        "Text ramp, no glyphs needed",
        "Eighth-height, smoothest tops"
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

static const char *const ON_HIT_NAME[]={"NOTHING","BUZZ","BUZZ + CATCH"};
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
        "Vibrate, tune to it, start WATCH"
    };
    ls_picker_open("ON DETECTION",on_hit_done);
    for(int i=0;i<3;i++)
        ls_picker_add(ON_HIT_NAME[i],i==now?"in use":WHY[i]);
}

/* STYLE and COLOUR are one question with two halves - how the spectrum is
   drawn - and they sat in MORE as two of nine rows, next to ALERTS and
   RECEIVER TOOLS, which are not about drawing anything. */
static void display_done(int index)
{
    if(index==0)style_open();
    else if(index==1)colour_open();
}
static void display_open(void)
{
    scan_look_load();
    ls_picker_open("DISPLAY",display_done);
    ls_picker_add("STYLE",SCAN_GRAIN_NAME[scan_grain]);
    ls_picker_add("COLOUR",SCAN_LOOK_NAME[scan_look]);
}

/* What the SCAN button offers while a sweep is running.

   Everything here is about the sweep in front of you, and none of it was
   reachable without going through MORE and reading past four rows that were
   not. Stopping in particular: a running sweep holds the radio, and the
   control for letting go of it belongs one press away. */
/* The bounds are the runtime's own, checked here so a refusal can say what
   was wrong instead of silently clamping a typed number to something else. */
static void scan_threshold_set(double db)
{
    if(!isfinite(db) || db<REC_SCAN_DETECT_MIN || db>REC_SCAN_DETECT_MAX) {
        snprintf(feedback,sizeof(feedback),"Enter %.0f-%.0f dB over the floor",
                 (double)REC_SCAN_DETECT_MIN,(double)REC_SCAN_DETECT_MAX);
        return;
    }
    rec_watch_scan_set_threshold((float)db);
    snprintf(feedback,sizeof(feedback),"Detect above %.0f dB over the floor",
             (double)rec_watch_scan_threshold());
}
static void sweep_menu_done(int index)
{
    if(index<0 || index>=menu_n)return;
    switch(menu_id[index]) {
    case 'X':
        rec_watch_scan_stop();
        snprintf(feedback,sizeof(feedback),"Stopping the sweep");
        break;
    case 'T':
        ls_numpad_open("DETECT ABOVE","dB over floor",
                       (double)rec_watch_scan_threshold(),scan_threshold_set);
        break;
    case 'H': on_hit_open(); break;
    case 'D': sweep_hits=!sweep_hits; break;
    case 'W': scan_open(); break;
    default: break;
    }
}
static void sweep_open(void)
{
    char thr[32];
    snprintf(thr,sizeof(thr),"%.0f dB over the floor",(double)rec_watch_scan_threshold());
    menu_n=0;
    ls_picker_open("SWEEP",sweep_menu_done);
    menu_add('X',"STOP","Give the radio back");
    menu_add('T',"THRESHOLD",thr);
    menu_add('H',"ON DETECT",ON_HIT_NAME[(int)rec_watch_scan_on_hit_get()]);
    menu_add('D',sweep_hits?"SHOW SPECTRUM":"SHOW DETECTIONS",
             sweep_hits?"Back to the band":"What it has found so far");
    menu_add('W',"SWEEP ELSEWHERE","Pick another range");
}

/* Which receiver is listening, as a list rather than a cycle.

   Cycling gave no way to see what the other two were before committing to
   one, and no room to say that the CC1101 is missing or that choosing the
   SX1262 takes the mesh down. */
static void source_done(int index)
{
    if(index<0 || index>=REC_SOURCE_COUNT)return;
    const rec_source_t want=(rec_source_t)index;
    if(want==rec_watch_source())return;
    if(rec_watch_enabled()){snprintf(feedback,sizeof(feedback),"Stop WATCH before changing source");return;}
    source_frequency[rec_watch_source()]=rec_get_freq();
    rec_disarm();
    if(rec_watch_select_source(want)) {
        rec_set_freq(source_frequency[want]);
        snprintf(feedback,sizeof(feedback),"%s selected; WATCH starts capture",source_name());
    } else {
        snprintf(feedback,sizeof(feedback),"%s would not start",rec_source_name(want));
    }
}
static void source_open(void)
{
    ls_mixrf_status_t cc;ls_mixrf_snapshot(&cc);
    const rec_source_t now=rec_watch_source();
    ls_picker_open("RECEIVER",source_done);
    for(int i=0;i<REC_SOURCE_COUNT;i++) {
        const char *why;
        switch((rec_source_t)i) {
        case REC_SOURCE_RTL:    why="USB dongle, wide tuning";break;
        /* Live state, not a capability list: a row that says CC1101 on a
           board with no CC1101 is the lit-control-that-does-nothing fault. */
        case REC_SOURCE_CC1101: why=cc.cc?"On board, OOK only":
                                     cc.busy?"Still probing":"Not detected";break;
        case REC_SOURCE_SX1262: why="Demodulates, and can transmit";break;
        default:                why="";break;
        }
        ls_picker_add(rec_source_name((rec_source_t)i),i==(int)now?"in use":why);
    }
}

/* Where to point it. A typed frequency, a preset and a peak from the last
   sweep are three ways of answering one question, and they were three
   separate controls - two on the bar and one buried in MORE. */
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
    if(rec_watch_scan_result(NULL,0)>=0)
        menu_add('V',"FROM THE LAST SWEEP","Tune to a peak it found");
}

static void more_menu_done(int index)
{
    if (index < 0 || index >= menu_n) return;
    switch (menu_id[index]) {
    case 'L': learn_signal_open(); break;
    case 'G': display_open(); break;
    case 'A': action('a'); break;
    case 'T': action('d'); break;
    case 'D': notes=!notes; break;
    default: break;
    }
}

static void action(char c)
{
    feedback[0]=0;
    /* Say why, then stop. Every branch below also guards itself, so this is
       about telling the operator rather than about safety. */
    {
        const char *no=disabled_reason(c);
        if(no){snprintf(feedback,sizeof(feedback),"%s",no);return;}
    }
    if(c=='w') {
        bool on=!rec_watch_enabled();
        if(rec_watch_enable(on)) {if(on && rec_watch_source()==REC_SOURCE_RTL){ls_tui_radio_want("REC");rec_arm_request();}else if(!on)rec_disarm();}
        else {
            rec_watch_snapshot(&s);
            /* The receiver that refused knows why; ask it before falling back
               on a sentence about two other receivers. The old message named
               the RTL and the CC1101 whatever had gone wrong, and on a third
               source it was both untrue and too long for the line. */
            const char *why=rec_watch_error();
            if(!s.ready) why="Still loading the archive from the card";
            else if(!why || !*why) switch(rec_watch_source()) {
                case REC_SOURCE_CC1101: why="No CC1101 - try PROBE in MIX-RF"; break;
                case REC_SOURCE_RTL:    why="No RTL receiver on USB"; break;
                default:                why="Receiver would not start"; break;
            }
            snprintf(feedback,sizeof(feedback),"%s",why);
        }
    } else if(c=='c') {
        if(selected>=s.count){snprintf(feedback,sizeof(feedback),"Nothing captured yet");return;}
        const rec_watch_event_t *e=&s.event[selected];
        char title[40];
        snprintf(title,sizeof(title),"CAPTURE #%lu",(unsigned long)e->id);
        menu_n=0;
        ls_picker_open(title,capture_menu_done);
        /* Offered only when it can actually be done. A capture from a
           receiver that cannot transmit, or one with no modulation recorded,
           has nothing to send it under. */
        if(rec_source_can_replay((rec_source_t)e->source) && e->bitrate)
            menu_add('R',"REPLAY","Send it again - pick the power");
        menu_add('S',"SAVE .SUB","Flipper file on the card");
        menu_add('P',e->pinned?"UNPIN":"PIN",e->pinned?"Allow eviction":"Keep it");
        menu_add('J',"JOURNAL","Mark with GPS and time");
    } else if(c=='m') {
        menu_n=0;
        /* Both of these start at -1 and are only settled by the spectrum, so
           a menu opened before the first sweep indexed the name tables at -1
           and read the element in front of them - which is the last entry of
           the table above, so STYLE showed ON DETECT's value and COLOUR
           showed STYLE's. */
        scan_look_load();
        ls_picker_open("MORE",more_menu_done);
        /* Short enough to survive the 52-column panel. The detail column is
           what is left after the label, and thirty-two is all a twelve
           character label leaves - a sentence that reaches the screen as
           "and appl" reads as a fault. */
        menu_add('L',"LEARN SIGNAL","Rate, deviation and sync");
        menu_add('A',"ALERTS",s.alerts?"Mesh DM on":"Mesh DM off");
        menu_add('G',"DISPLAY","How the spectrum is drawn");
        menu_add('D',"NOTES",notes?"Hide the legend":"Explain the list");
        /* Only for the source it belongs to. Listed unconditionally it was a
           row that could do exactly one thing - print a sentence saying it
           was the wrong source - which is a menu entry that exists to refuse
           itself. */
        if(rec_watch_source()==REC_SOURCE_RTL)
            menu_add('T',"RECEIVER TOOLS","RTL level and rate");
    } else if(c=='i') {notes=!notes;
    } else if(c=='n') {
        if(rec_watch_scan_busy())sweep_open();
        else scan_open();
    } else if(c=='f') tune_open();
    else if(c=='p' && selected<s.count) {
        if(!rec_watch_request_pin(s.event[selected].id,!s.event[selected].pinned))
            snprintf(feedback,sizeof(feedback),"Pin request busy");
    } else if(c=='a') {
        memset(peers,0,sizeof(peers));
        ls_picker_open("DETECTION DM TARGET",peer_done);
        ls_picker_add("OFF","No detection messages");
        ls_mesh_peer_t peer;
        for(int j=0;j<LS_MESH_MAX_PEERS && ls_mesh_peer_at(j,&peer);j++) {
            snprintf(peers[j],sizeof(peers[j]),"%s",peer.id);
            ls_picker_add(peer.name[0]?peer.name:peer.id,"New patterns; max one/min; Mesh TX must be armed");
        }
    } else if(c=='e' && selected<s.count) {
        if(!rec_watch_request_export(s.event[selected].id)) snprintf(feedback,sizeof(feedback),"Export busy or pattern unavailable");
    } else if(c=='j' && selected<s.count) {
        const rec_watch_event_t *e=&s.event[selected];
        char title[48],text[480];
        snprintf(title,sizeof(title),"SubGHz pattern #%lu",(unsigned long)e->id);
        /* What actually made the recording. "OOK timing pattern" was printed
           for every source, including the one that demodulates rather than
           timing edges - a note that misdescribes its own capture is worse
           than one with no description. */
        snprintf(text,sizeof(text),"%s; not a decoded device identity. "
            "%.4f MHz, %u edges, %.2f ms, %lu observations. "
            "First: boot %08lx uptime %llu ms. Last: boot %08lx uptime %llu ms. "
            "GPS and motion attachment describe this bookmark, not the original capture. "
            "Export the representative from SUB-GHZ for pulse data.",
            e->deviation_hz?"Demodulated 2-FSK bitstream":"OOK timing pattern",
            e->frequency/1e6,e->edges,e->span_us/1000.,
            (unsigned long)e->count,(unsigned long)e->first_boot,(unsigned long long)e->first_ms,
            (unsigned long)e->last_boot,(unsigned long long)e->last_ms);
        size_t used=strlen(text);
        if(s.decoded[selected].repeats)snprintf(text+used,sizeof(text)-used," OOK24 payload %06lX, %u matching frames.",(unsigned long)s.decoded[selected].value,s.decoded[selected].repeats);
        bool ok=ls_field_mark_radio(title,text,e->source==REC_SOURCE_CC1101?LS_FIELD_CC1101:e->source==REC_SOURCE_SX1262?LS_FIELD_LORA:LS_FIELD_RTL,e->frequency,e->count);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy; try again shortly");
    } else if(c=='d') {if(rec_watch_source()==REC_SOURCE_RTL && !rec_watch_enabled())ls_scr_rec_tools();
    } else if(c=='s') {
        /* No CC1101 branch: disabled_reason('s') has already said its
           settings are fixed and returned, so the one that used to be here
           printed a sentence nothing could reach. */
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
static void enter(void) { button_focus=-1;button_slot=0;rec_watch_start();ls_field_start();ls_field_watch(true);feedback[0]=0;}
static void leave(void) {ls_field_watch(false);}
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
    snprintf(text,sizeof(text),"%s #%lu / %s",rec_source_name((rec_source_t)e->source),(unsigned long)e->id,
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
    snprintf(text,sizeof(text),"0 -> %.2f ms | first %d/%u edges",pulse_total/1000.,n,e->edges);
    tui_put_str(sf,a,a.x+2,a.y+6,text,LS_ATTR_DIM);
    snprintf(text,sizeof(text),"Tap trace: #%d %s %lu us",pulse_selected+1,s.preview[selected][pulse_selected]>0?"HIGH":"LOW",(unsigned long)(s.preview[selected][pulse_selected]<0?-(int64_t)s.preview[selected][pulse_selected]:s.preview[selected][pulse_selected]));
    tui_put_str(sf,a,a.x+2,a.y+7,text,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    if(a.h>10) {
        snprintf(text,sizeof(text),"Range %lu..%lu us / %s",(unsigned long)low,(unsigned long)high,e->end_reason==REC_END_GAP?"GAP end":"LIMIT end");
        tui_put_str(sf,a,a.x+2,a.y+8,text,LS_ATTR_DIM);
    }
    if(a.h>12)tui_put_str(sf,a,a.x+2,a.y+10,"| = transition(s) within one time cell",LS_ATTR_DIM);
    if(a.h>11 && s.decoded[selected].repeats) {
        snprintf(text,sizeof(text),"OOK24 %06lX / %u frames / %u us unit",(unsigned long)s.decoded[selected].value,s.decoded[selected].repeats,s.decoded[selected].unit_us);
        tui_put_str(sf,a,a.x+2,a.y+9,text,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    }
}
static void draw(tui_surface *sf,tui_rect a)
{
    if(a.w<24 || a.h<17) {ls_panel_notice(sf,a,"SUB-GHZ","Enlarge the pane","WATCH keeps its state");return;}
    rec_watch_snapshot(&s);
    uint8_t fresh=ls_fresh(&arrivals,s.received,600);
    touch_nav=a.w<90 && a.h>35;
    /* A LANDSCAPE PANE HAS A THIRD OF THE ROWS AND THE SAME THINGS TO SHOW.

       Controls take the short form here - three rows, label and value on one
       line inside the frame - which is what ls_btn_raised_height returns for
       a pane twice as wide as it is tall and what every other screen's bar
       already looks like in this posture. This one asked for four instead,
       and a second rule then made it SIX whenever the pane was short: taller
       controls the less height there was to spare, which is backwards. With
       the bottom bar's hardcoded six that was twelve of thirty-one rows
       spent on buttons and thirteen left for the captures.

       Three rows only where the buttons fit it. The old comment here was
       right that the compact form can clip WATCH OFF - just not about when.
       It happens on a HALF-WIDTH landscape split, where five buttons in
       fifty-six columns leave eight columns for the text; a full pane leaves
       thirteen, and the longest pair, SETUP CAPTURE, is thirteen.
       ls_btn_compact_fits measures it with the renderer's own numbers, so
       neither of us has to guess, and a wider label later cannot quietly
       start cutting itself in half. */
    const bool wide=a.w>a.h*2;

    if(selected>=s.count)selected=s.count?s.count-1:0;
    rec_hub_status_t rx;rec_get_hub_status(&rx);
    ls_mixrf_status_t cc;ls_mixrf_snapshot(&cc);
    bool cc_source=rec_watch_source()==REC_SOURCE_CC1101;
    if(cc_source){rx.freq_hz=rec_get_freq();rx.receiver_streaming=cc.capturing && cc.receiving;}
    /* In the order the job is done: point the receiver, start it, then act
       on what it heard. The old order was whatever each control was added in,
       which is why the screen read as a pile of buttons rather than a flow.
       Disabled state is computed once here and explained by disabled_reason,
       so the two cannot drift apart. */
    ls_btn_t btn[]={
        /* point the receiver */
        {"TUNE","MHz",'f',false,disabled_reason('f')!=NULL},
        {"SETUP","CAPTURE",'s',false,disabled_reason('s')!=NULL},
        /* run it - either by listening on one frequency or by looking
           across a range for one worth listening to */
        {"WATCH",s.enabled?"ON":"OFF",'w',s.enabled,disabled_reason('w')!=NULL},
        {"SCAN",rec_watch_scan_busy()?"ON":"OFF",'n',rec_watch_scan_busy(),
         disabled_reason('n')!=NULL},
        /* and everything you do to what it heard */
        {"CAPTURE",s.count?"MENU":"NONE",'c',false,!s.count}};
    /* The bottom bar. SOURCE is always on it; the pattern nav joins it only
       on a pane too narrow to walk the list another way. SOURCE is index 0 in
       both shapes, so the touch dispatch never has to ask which shape it is
       looking at. */
    {
        ls_btn_t bottom[4];
        int n=0;
        bottom[n++]=(ls_btn_t){"SOURCE",source_face(&cc),'r',false,s.enabled};
        bottom[n++]=(ls_btn_t){"MORE","OPTIONS",'m',false,false};
        if(touch_nav) {
            bottom[n++]=(ls_btn_t){"PREV","PATTERN",',',false,!s.count || selected==0};
            bottom[n++]=(ls_btn_t){"NEXT","PATTERN",'.',false,selected+1>=s.count};
        }
        /* Short in landscape, and the same height portrait had otherwise -
           portrait has the rows to spare and the fatter target is worth
           them. Two buttons in a wide pane always fit the compact form, but
           it is asked rather than assumed, because the row is built above
           and PREV/NEXT can join it. */
        const int bh=wide?(ls_btn_compact_fits(tui_rect_make(a.x,a.y,a.w,3),bottom,n)?3:4)
                         :touch_nav?5:6;
        if(a.h>bh+10) {
            ls_btn_bar_raised_slot(sf,tui_rect_make(a.x,a.y+a.h-bh,a.w,bh),bottom,n,-1,LS_BTN_SLOT_WATERFALL);
            a.h-=bh;
        }
    }

    /* State first. Two rows, and they come out of the list rather than out
       of the button bar - a screen that cannot say whether it is running is
       worse than one with a shorter list. */
    const int banner_h = a.h >= 20 ? 2 : 0;
    if (banner_h) {
        if (s.enabled && !listening_since) listening_since = esp_timer_get_time();
        if (!s.enabled) listening_since = 0;
        /* Inset by a column each side: the pane's edge columns belong to
           whatever box is drawn around the screen, and a fill that lands
           in them breaks the frame rather than decorating it. */
        state_banner(sf, tui_rect_make(a.x + 1, a.y, a.w - 2, banner_h),
                     &s, &rx, esp_timer_get_time(), fresh);
        a.y += banner_h; a.h -= banner_h;
    }
    /* The shared rule, which already answers three for a wide pane and
       seven for a tall one. The old comment here said three rows clipped
       WATCH OFF to WATCH OF; at this width it does not - the compact form
       gets box.w-2 columns and the longest pair, SETUP CAPTURE, is
       thirteen. test_tui_screens holds that measurement so a wider label
       cannot quietly start clipping again. */
    int h=wide?(ls_btn_compact_fits(tui_rect_make(a.x,a.y,a.w,3),btn,5)?3:4)
              :ls_btn_raised_height(a,5);
    /* A tall narrow split: two columns of buttons, so it needs the rows
       for two of them. */
    if(a.w>=32 && a.w<38 && a.h>=36) h=16;
    ls_btn_bar_raised(sf,tui_rect_make(a.x,a.y,a.w,h),btn,5,button_focus);
    tui_rect body=tui_rect_make(a.x,a.y+h,a.w,a.h-h-1);
    tui_rect content=tui_rect_make(body.x+1,body.y+1,body.w-2,body.h-2);
    /* Named from the source rather than from a two-way ternary, which said
       RTL for anything that was not the CC1101 - including a receiver that
       is neither, and does not do OOK. */
    /* And the modulation from the source too. The CC1101 and the RTL time
       edges - that is amplitude keying whatever is on the air - and the
       SX1262 demodulates, so it is the only one that can say 2-FSK. A
       two-way ternary over three sources gets one of them wrong. */
    char panel_title[48];
    snprintf(panel_title,sizeof(panel_title),"%s WATCH / %s %s",
             rec_watch_source()==REC_SOURCE_SX1262?"":"PASSIVE",
             source_name(),
             rec_watch_source()==REC_SOURCE_SX1262?"2-FSK":"OOK");
    ls_panel_box(sf,body,panel_title,TUI_CYAN);
    ls_motion_busy(sf,body,s.exporting ||
                   (s.enabled && rx.receiver_streaming));
    /* While it is sweeping, the band IS the screen. Everything below is
       about captures, and there are none yet. */
    /* NOT an early return.

       Returning here skipped the button bars, which meant that while a sweep
       was running the pane had no controls on it at all - no way to stop it
       by touch, and a screen with nothing pressable on it reads as a hung
       one whatever it is drawing. The sweep takes over the LIST, which has
       nothing in it yet anyway, and leaves everything else alone. */
    const bool sweeping=rec_watch_scan_busy();
    const bool learning=rec_watch_learn_busy();
    char line[110];
    /* Frequency, state, health and storage all moved into the banner. What is
       left here is the part that is about the SELECTED capture rather than
       about the session. */
    if(!banner_h) {
        snprintf(line,sizeof(line),"%c %.4f MHz | %s",ls_motion_pip(s.enabled && rx.receiver_streaming),
            rx.freq_hz/1e6,!s.enabled?"STOPPED":rx.receiver_streaming?"LISTENING":"RX UNAVAILABLE");
        tui_put_str(sf,content,body.x+2,body.y+1,line,LS_ATTR_DIM);
    }
    if(cc_source && cc.raw_overflows) {
        snprintf(line,sizeof(line),"CC buffer overflows %lu",(unsigned long)cc.raw_overflows);
        tui_put_str(sf,content,body.x+2,body.y+2,line,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    }
    if(s.count) {
        const rec_watch_event_t *e=&s.event[selected];
        snprintf(line,sizeof(line),"LAST up %.3fs",e->last_ms/1000.0);
    } else snprintf(line,sizeof(line),"LAST -- / no captures");
    tui_put_str(sf,content,body.x+2,body.y+3,line,LS_ATTR_DIM);
    list=tui_rect_make(body.x+2,body.y+4,body.w-4,body.h-6);
    if(body.h<12) list.h=body.h-6;
    /* A sweep or a learn owns the list area - there is nothing in the list
       yet, and this is what the operator is actually watching. The button
       bars above and below are untouched, so the sweep can be stopped by
       touch and the screen never looks dead. */
    if(sweeping) {
        hit_flash_poll();
        /* SCAN answers "what has it actually found" as well as showing the
           band, and the spectrum is one press away again. */
        if(sweep_hits) hits_table(sf,tui_rect_make(body.x+2,body.y+2,body.w-4,body.h-3));
        else spectrum(sf,tui_rect_make(body.x+2,body.y+3,body.w-4,body.h-4));
        ls_safe_line(sf,a,a.y+a.h-1,
            feedback[0]?feedback:sweep_hits?"SCAN > SHOW SPECTRUM returns to the band":s.export_status,
            LS_ATTR_DIM);
        return;
    }
    /* The hits table used to keep the pane after the sweep ended. The only
       control that turned it off lived in the SWEEP menu, which is only
       reachable while a sweep is running - so a sweep that finished while it
       was on left the screen showing a table with no way back to the capture
       list. The findings are still there: TUNE offers them as somewhere to
       tune to, which is what they were for. */
    sweep_hits=false;
    if(learning) {
        tui_rect w=tui_rect_make(body.x+2,body.y+4,body.w-4,body.h-6);
        char head[72];
        snprintf(head,sizeof(head),"%s",rec_watch_scan_stage());
        tui_put_str(sf,w,w.x,w.y,head[0]?head:"Working",
                    TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        const int pct=rec_watch_scan_progress();
        char pctxt[8];snprintf(pctxt,sizeof(pctxt),"%d%%",pct);
        if((int)strlen(pctxt)+2<w.w)
            tui_put_str(sf,w,w.x+w.w-(int)strlen(pctxt),w.y,pctxt,
                        TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        if(w.w>8 && w.h>3) {
            const int full=(w.w-2)*pct/100;
            for(int i=0;i<w.w-2;i++)
                tui_put_char(sf,w,w.x+1+i,w.y+2,
                    i<full?LS_TUI_SHADE_75:LS_TUI_SHADE_25,
                    TUI_ATTR((i<full?TUI_GREEN:TUI_CYAN)|TUI_BRIGHT,TUI_BLACK));
        }
        if(w.h>5) tui_put_str(sf,w,w.x,w.y+4,"Keep pressing the transmitter",LS_ATTR_DIM);
        ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:s.export_status,LS_ATTR_DIM);
        return;
    }
    if(body.w>90 && body.h>12) {
        list.w=(body.w-6)/2;
        waveform(sf,tui_rect_make(list.x+list.w+2,body.y+3,body.w-list.w-5,body.h-4));
    } else if(body.h>24) {
        list.h=(body.h-18)/2*2;
        waveform(sf,tui_rect_make(body.x+1,list.y+list.h+1,body.w-2,body.h-list.h-10));
    }
    int rows=list.h/2;if(rows<1)rows=1;
    int first=selected/rows*rows;
    if(!s.count)tui_put_str(sf,content,list.x,list.y,"No patterns for this receiver. Start WATCH.",LS_ATTR_DIM);
    for(int i=0;i<rows && first+i<s.count;i++) {
        const rec_watch_event_t *e=&s.event[first+i];int y=list.y+i*2;
        if(first+i==selected)ls_fill_dither(sf,tui_rect_make(list.x,y,list.w,2),LS_DITHER_LIGHT,TUI_CYAN);
        snprintf(line,sizeof(line),"%s #%lu %.4fMHz x%lu",e->last_boot==s.boot_id?"[RX]":"[SD]",(unsigned long)e->id,e->frequency/1e6,(unsigned long)e->count);
        tui_put_str(sf,list,list.x,y,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        if(s.decoded[first+i].repeats)snprintf(line,sizeof(line),"%s OOK24 %06lX / %u repeats",rec_source_name((rec_source_t)e->source),(unsigned long)s.decoded[first+i].value,s.decoded[first+i].repeats);
        else snprintf(line,sizeof(line),"%s %u edges %.1fms",e->end_reason!=REC_END_GAP?"LIMIT":e->count>1?"REPEAT":"RAW?",e->edges,e->span_us/1000.);
        tui_put_str(sf,list,list.x,y+1,line,LS_ATTR_DIM);
    }
    if(notes) {
        tui_rect info=tui_rect_make(body.x+1,body.y+3,body.w-2,body.h-4);
        tui_fill(sf,info,' ',LS_ATTR_DIM);
        ls_panel_box(sf,info,"WHAT THE LIST MEANS",TUI_CYAN);
        tui_put_str(sf,info,info.x+2,info.y+1,s.storage,LS_ATTR_DIM);
        snprintf(line,sizeof(line),"Loss: %lu skipped / CC buffer overflows %lu",(unsigned long)s.dropped,(unsigned long)cc.raw_overflows);
        tui_put_str(sf,info,info.x+2,info.y+2,line,LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+3,"RAW? unverified / REPEAT timing match",LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+4,"[SD] earlier boot / [RX] this boot",LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+5,"Pattern match is not a device identity.",LS_ATTR_DIM);
        /* Named from the source. A two-way ternary said RTL for the
           SX1262 as well, describing a receiver that times OOK edges on the
           one that demodulates. */
        tui_put_str(sf,info,info.x+2,info.y+6,
            cc_source?"CC: 650kHz OOK / 40us filter / 30ms gap":
            rec_watch_source()==REC_SOURCE_SX1262?"SX1262: 2-FSK, rate and deviation in SETUP":
            "RTL: configurable OOK pulse capture",LS_ATTR_DIM);
        snprintf(line,sizeof(line),"DM queued %lu / refused %lu / limited %lu",(unsigned long)s.alert_sent,(unsigned long)s.alert_failed,(unsigned long)s.alert_suppressed);
        tui_put_str(sf,info,info.x+2,info.y+7,line,LS_ATTR_DIM);
    }
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:s.export_status,LS_ATTR_DIM);

}
static bool key(ls_tk_t k,char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if(k==LS_TK_LEFT || k==LS_TK_RIGHT || k==LS_TK_TAB)return ls_btn_navigate(k,&button_slot,&button_focus,false);
    if(k==LS_TK_ENTER && button_focus>=0){
            if(button_focus<(int)sizeof(BTN_KEYS)-1)action(BTN_KEYS[button_focus]);
        return true;
    }
    if(k==LS_TK_UP || k==LS_TK_DOWN || k==LS_TK_BACKSPACE)button_focus=-1;
    if(k==LS_TK_UP){if(selected)selected--;return true;}
    if(k==LS_TK_DOWN){if(selected+1<s.count)selected++;return true;}
    if(k!=LS_TK_CHAR || !ch)return false;
    if(ch==','){if(selected)selected--;return true;}if(ch=='.'){if(selected+1<s.count)selected++;return true;}
    if(ch=='r'){
        const char *no=disabled_reason('r');
        if(no){snprintf(feedback,sizeof(feedback),"%s",no);return true;}
        source_open();return true;
    }
    /* Every action keeps its letter whether or not it is still on the bar -
       a shortcut that stopped working because a button moved into a menu
       would be a worse trade than the crowding it fixed. */
    if(!strchr("fswncmpejadi",ch))return false;
    action(ch);return true;
}
static bool touch(int x,int y)
{
    /* The threshold control first: it sits inside the spectrum, which is
       drawn over the area the capture list would otherwise own. */
    if(rec_watch_scan_busy() && gate_hit.h>0 && tui_rect_contains(gate_hit,x,y)) {
        /* Straight to the height that was tapped, on the plot's own scale -
           the bottom of the track is the floor and the top is whatever the
           display is currently showing as full height. */
        const int from_bottom=gate_hit.y+gate_hit.h-1-y;
        float db=gate_hit_span*(float)from_bottom/(float)gate_hit.h;
        rec_watch_scan_set_threshold(db);
        snprintf(feedback,sizeof(feedback),"Detect above %.0f dB over the floor",
                 (double)rec_watch_scan_threshold());
        return true;
    }
    {
        const int i=ls_btn_hit_slot(x,y,LS_BTN_SLOT_WATERFALL);
        if(i==0)return key(LS_TK_CHAR,'r');
        if(i==1)return key(LS_TK_CHAR,'m');
        if(i>1){if(touch_nav)return key(LS_TK_CHAR,i==2?',':'.');return true;}
    }
    {
            const int i=ls_btn_hit(x,y);
        if(i>=0){if(i<(int)sizeof(BTN_KEYS)-1)action(BTN_KEYS[i]);return true;}
    }
    if(!notes && pulse_total && tui_rect_contains(pulse_hit,x,y)) {
        uint64_t t=(uint64_t)(x-pulse_hit.x)*pulse_total/pulse_hit.w,sum=0;
        int n=s.event[selected].edges;if(n>48)n=48;
        for(int j=0;j<n;j++) {sum+=s.preview[selected][j]<0?-(int64_t)s.preview[selected][j]:s.preview[selected][j];if(sum>t){pulse_selected=j;break;}}
        return true;
    }
    if(x>=list.x && x<list.x+list.w && y>=list.y && y<list.y+list.h) {
        int rows=list.h/2;if(rows<1)rows=1;
        int n=selected/rows*rows+(y-list.y)/2;if(n<s.count)selected=n;
    }
    return true;
}
const ls_tui_screen_t ls_scr_subghz={.radio="REC",.name="SUB-GHZ",.hint="W watch  N scan  F tune  C capture",.enter=enter,.leave=leave,.draw=draw,.key=key,.touch=touch};
