#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_numpad.h"
#include "../../ls_picker.h"
#include "../../ls_radio_select.h"
#include "../../ls_options.h"
#include "../../ls_motion.h"
#include "../../ls_field.h"
#include "rec_state.h"
#include "rec_watch.h"
#include "ls_mesh.h"
#include "ls_mixrf.h"
#include "../../ls_waterfall.h"
#include "../../ls_tui_touch.h"
#include "../../ls_icons.h"
#include "../../ls_rec_replay.h"
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
static float gate_hit_span;
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
static int det_row_n;
static bool notes;        /* capture list: the legend overlay              */
static ls_fresh_t arrivals;
static int setup_item;
/* Sized off the enum, not off how many sources there happened to be when
   this was written. */
static uint32_t source_frequency[REC_SOURCE_COUNT]={433920000,433920000,433920000};
/* The capture source as the RADIO picker counts it, and its name there. */
static ls_rsel_radio_t source_radio(void) { return rec_source_radio(rec_watch_source()); }
static const char *source_name(void) { return ls_rsel_name(source_radio()); }

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
    const int events = rec_watch_scan_events();
    det_hit = tui_rect_make(0, 0, 0, 0);
    if (rec_watch_scan_busy() || events) {
        /* A chip, because it is a control: it opens the detections. It
           blinks with every new burst, so a count going up is seen rather
           than read. */
        static EXT_RAM_BSS_ATTR rec_scan_bin_t tmp[REC_SCAN_BINS];
        const int freqs = rec_watch_scan_result(tmp, REC_SCAN_BINS);
        snprintf(line, sizeof(line), " %d DETECTED  %d FREQ  %s ",
                 events, freqs > 0 ? freqs : 0, sweep_hits ? "CLOSE" : "VIEW >");
        const bool blink = hit_flash_on();
        const uint8_t hue = events ? TUI_GREEN : TUI_CYAN;
        const uint8_t at = blink ? TUI_ATTR(TUI_BLACK, TUI_WHITE | TUI_BRIGHT)
                                 : TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT);
        tui_put_str(sf, r, r.x + 1, r.y + 1, line, at);
        det_hit = tui_rect_make(r.x, r.y, (int)strlen(line) + 2, 2);
    } else {
        snprintf(line, sizeof(line), "%lu:%02lu   %d capture%s   heard %lu",
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
    char title[24];
    snprintf(title,sizeof(title),"%s RECEIVE",ls_rsel_name(LS_RSEL_LORA));
    ls_picker_open(title,fsk_done);
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
    case 'w': return !s.ready?"Still loading capture history from the card":
                     !watching && rec_watch_scan_busy()?"Stop SCAN before WATCH":NULL;
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
    case 'd': {
        EXT_RAM_BSS_ATTR static char rtl_only[48];
        snprintf(rtl_only,sizeof(rtl_only),"READ RAW records with the %s",ls_rsel_name(LS_RSEL_SDR_RTL));
        return rec_watch_source()!=REC_SOURCE_RTL?rtl_only:
               watching?"Stop WATCH in READ first":NULL;
    }
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
static const int OOK_DBM[] = {-10, 0, 5, 10};
static void replay_power_done(int index)
{
    if (index < 0 || index >= (int)(sizeof(REPLAY_DBM)/sizeof(REPLAY_DBM[0]))) return;
    rec_watch_snapshot(&s);
    if (selected >= s.count) return;
    const rec_watch_event_t *e = &s.event[selected];
    const int dbm=e->source==REC_SOURCE_CC1101?OOK_DBM[index]:REPLAY_DBM[index];
    if (!rec_watch_request_replay(e->id, dbm))
        snprintf(feedback,sizeof(feedback),"Stop WATCH before replaying");
    else
        snprintf(feedback,sizeof(feedback),"Sending #%lu at %d dBm...",
                 (unsigned long)e->id, dbm);
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
    if (selected<s.count && s.event[selected].source==REC_SOURCE_CC1101) {
        static const char *const O[]={"-10 dBm","0 dBm","5 dBm","10 dBm"};
        ls_picker_open("CC1101 NOMINAL POWER",replay_power_done);
        for(int i=0;i<4;i++)ls_picker_add(O[i],"Send RAW OOK once");
        return;
    }
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

/* WHAT IT ACTUALLY FOUND, as a table rather than a picker.

   The old answer was a list of frequencies, which cannot distinguish a
   transmitter that spoke once from one that is still going. Count and
   age are what make a detection worth acting on. */
static void hits_table(tui_surface *sf, tui_rect r)
{
    static EXT_RAM_BSS_ATTR rec_scan_bin_t hit[REC_SCAN_BINS];
    int n = rec_watch_scan_result(hit, REC_SCAN_BINS);
    if (n < 0) n = 0;
    /* Strongest first. A finished sweep is already sorted; a running one
       is in the order things were found. */
    for (int i = 1; i < n; i++) {
        const rec_scan_bin_t k = hit[i];
        int j = i - 1;
        while (j >= 0 && hit[j].dbm < k.dbm) { hit[j + 1] = hit[j]; j--; }
        hit[j + 1] = k;
    }
    det_row_n = 0;
    const float floor_dbm = rec_watch_scan_busy() ? rec_watch_scan_live_floor()
                                                  : rec_watch_scan_floor();
    char line[110];

    snprintf(line, sizeof(line), "DETECTIONS  %d bursts on %d freq, over %.0f dB",
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
    snprintf(line, sizeof(line), "%-11s %7s %5s %5s %8s",
             "MHz", "dBm", "over", "seen", "last");
    tui_put_str(sf, r, r.x, r.y + 1, line, LS_ATTR_DIM);

    const int64_t now = esp_timer_get_time();
    const int rows = r.h - 3;
    det_rows = tui_rect_make(r.x, r.y + 2, r.w, rows < n ? rows : n);
    det_row_n = 0;
    for (int i = 0; i < n && i < rows; i++) {
        if (det_row_n < DET_ROWS_MAX) det_row_hz[det_row_n++] = hit[i].hz;
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
    tui_put_str(sf, r, r.x, r.y + r.h - 1,
                n > rows ? "Tap a row to act on it - more below"
                         : "Tap a row to act on it",
                LS_ATTR_DIM);
}

/* Columns left of the track that still grab it. */
#define GATE_REACH 5

/* The threshold as a height on the plot, from a row inside gate_hit. */
static float gate_db_at_row(int y)
{
    if (y < gate_hit.y) y = gate_hit.y;
    if (y > gate_hit.y + gate_hit.h - 1) y = gate_hit.y + gate_hit.h - 1;
    const int from_bottom = gate_hit.y + gate_hit.h - 1 - y;
    return gate_hit_span * (float)from_bottom / (float)gate_hit.h;
}
static void gate_set_from_row(int y) { rec_watch_scan_set_threshold(gate_db_at_row(y)); }

/* A finger that went down on the track drags the line with it, frame by
   frame, until it lifts - wherever it wanders, up or down.

   The line moves in memory while the finger is down and is saved once when
   it lifts: a save per frame would be a flash write every 40 ms. */
static bool gate_dragging;
static void gate_follow_drag(void)
{
    int sc, sr, c, r;
    const bool held = gate_hit.h > 0 && ls_tui_touch_held(&sc, &sr, &c, &r) &&
                      tui_rect_contains(gate_hit, sc, sr);
    if (!held) {
        if (gate_dragging) rec_watch_scan_set_threshold(rec_watch_scan_threshold());
        gate_dragging = false;
        return;
    }
    gate_dragging = true;
    rec_watch_scan_preview_threshold(gate_db_at_row(r));
    snprintf(feedback, sizeof(feedback), "Detect above %.0f dB over the floor",
             (double)rec_watch_scan_threshold());
}

static void spectrum(tui_surface *sf, tui_rect r)
{
    scan_look_load();
    gate_follow_drag();
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
    ls_safe_line(sf,r,r.y,"",LS_ATTR_DIM);
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
        /* The target is wider than the track: the track sits against the
           panel's edge, where a fingertip lands short of it. */
        gate_hit = tui_rect_make(gx - GATE_REACH, top_row, gw + GATE_REACH, rows);
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
            tui_put_char(sf, r, x, r.y + r.h - 3, LS_TUI_SHADE_75,
                         TUI_ATTR(scan_hue(KEY[i].d) | TUI_BRIGHT, TUI_BLACK));
            tui_put_str(sf, r, x + 1, r.y + r.h - 3, KEY[i].t, LS_ATTR_DIM);
            x += w;
        }
    }
}

static uint32_t det_selected_hz;
static void detection_done(int index)
{
    if (index < 0 || index >= menu_n || !det_selected_hz) return;
    const uint32_t hz = det_selected_hz;
    switch (menu_id[index]) {
    case 'C':
        if (rec_watch_scan_busy()) {
            if (rec_watch_scan_catch(hz))
                snprintf(feedback, sizeof(feedback), "Stopping the sweep to capture %.4f MHz", hz / 1e6);
            sweep_hits = false;
            return;
        }
        if (rec_watch_enabled()) {
            snprintf(feedback, sizeof(feedback), "Stop WATCH before retuning");
            return;
        }
        source_frequency[rec_watch_source()] = hz;
        rec_set_freq(hz);
        sweep_hits = false;
        action('w');
        break;
    case 'T':
        if (rec_watch_scan_busy() || rec_watch_enabled()) {
            snprintf(feedback, sizeof(feedback), "Stop the sweep or WATCH before tuning");
            return;
        }
        source_frequency[rec_watch_source()] = hz;
        rec_set_freq(hz);
        sweep_hits = false;
        snprintf(feedback, sizeof(feedback), "Tuned to %.4f MHz", hz / 1e6);
        break;
    case 'N': {
        /* Look closer: a narrow sweep centred on it, where nearly every
           burst lands. */
        if (rec_watch_scan_busy()) { rec_watch_scan_stop(); snprintf(feedback, sizeof(feedback), "Stop the sweep, then ZOOM again"); return; }
        const uint32_t half = 150000u;
        if (rec_watch_request_scan(hz > half ? hz - half : hz, hz + half, 0, 8))
            snprintf(feedback, sizeof(feedback), "Sweeping %.4f MHz +/-150 kHz", hz / 1e6);
        sweep_hits = false;
        break;
    }
    default: break;
    }
}
static void detection_open(uint32_t hz)
{
    static EXT_RAM_BSS_ATTR rec_scan_bin_t bins[REC_SCAN_BINS];
    int n = rec_watch_scan_result(bins, REC_SCAN_BINS);
    const rec_scan_bin_t *b = NULL;
    for (int i = 0; i < n; i++) if (bins[i].hz == hz) b = &bins[i];
    char title[40], seen[40];
    snprintf(title, sizeof(title), "DETECTION %.4f MHz", hz / 1e6);
    if (b) snprintf(seen, sizeof(seen), "%.0f dBm peak, seen %lu times", (double)b->dbm, (unsigned long)b->seen);
    else seen[0] = 0;
    det_selected_hz = hz;
    menu_n = 0;
    ls_picker_open(title, detection_done);
    /* Capture is what turns a detection into something that can be looked
       at, replayed or sent: a sweep only knows that energy was there. */
    menu_add('C', "CAPTURE IT", rec_watch_scan_busy() ? "Stop, tune, start WATCH" : "Tune and start WATCH");
    menu_add('T', "TUNE HERE", seen[0] ? seen : "Point the receiver at it");
    menu_add('N', "ZOOM IN", "Sweep +/-150 kHz around it");
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
static void scan_range_done(int index)
{
    if(index<0 || index>=scan_at_rows)return;
    index=scan_at_row[index];
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
    /* WATCH is the usual reason and the only one the operator can act on,
       but it is not the only one - the receiver can simply refuse. Saying
       "stop WATCH" to somebody who is not watching sends them looking for a
       button that is already off. */
    if(!rec_watch_request_scan(lo,hi,0,bins))
        snprintf(feedback,sizeof(feedback),"%s",
                 rec_watch_enabled()?"Stop WATCH before scanning":
                                     "The receiver would not start a sweep");
    else {
        /* A new sweep opens on the band, not on what the last one found. */
        sweep_hits=false;
        snprintf(feedback,sizeof(feedback),"Sweeping %.3f-%.3f MHz, %d bins - SCAN again to stop",
                 lo/1e6,hi/1e6,bins);
    }
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
    if(rec_watch_enabled()) {
        ls_rsel_set(LS_RSEL_SUBGHZ_READ,rec_source_radio(now));
        snprintf(feedback,sizeof(feedback),"Stop WATCH before changing source");
        return false;
    }
    source_frequency[now]=rec_get_freq();
    rec_disarm();
    if(rec_watch_select_source(want)) {
        rec_set_freq(source_frequency[want]);
        ls_rsel_set(LS_RSEL_SUBGHZ_READ,radio);
        snprintf(feedback,sizeof(feedback),"%s selected; WATCH starts capture",source_name());
        return true;
    }
    ls_rsel_set(LS_RSEL_SUBGHZ_READ,rec_source_radio(now));
    snprintf(feedback,sizeof(feedback),"%s would not start",ls_rsel_name(radio));
    return false;
}
static void source_done(ls_rsel_radio_t radio) { (void)ls_scr_subghz_choose_source(radio); }
static void source_open(void) { ls_rsel_open(LS_RSEL_SUBGHZ_READ,source_done); }

/* ANALYZER's radio. Only the LoRa chip sweeps here, so the list is there to
   say so for the rest; choosing it changes nothing. */
static void sweep_done(ls_rsel_radio_t radio)
{
    snprintf(feedback,sizeof(feedback),"Sweeps on the %s",ls_rsel_name(radio));
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
    if(rec_watch_scan_result(NULL,0)>0)
        menu_add('V',"FROM THE LAST SWEEP","Tune to a peak it found");
}

/* ONE HOME, AND EVERYTHING ONE PRESS FROM IT.

   This screen used to open on five buttons, a list and a MORE menu, and the
   parts of the job that were not on the bar lived in other apps: receiver
   tools in REC, saved files in FILES, a file's replay back in REC again -
   each one a screen with no way back to this one. Now it opens on a page of
   six tiles, one per job, the way a Flipper's Sub-GHz menu does, and every
   page has BACK to here. Nothing moved out of the app to get there. */
typedef enum {
    PG_HOME,
    PG_READ,       /* listen on one frequency, keep what is heard        */
    PG_ANALYZE,    /* sweep a band to find the frequency                 */
    PG_SAVED,      /* what was caught, and the .sub files on the card    */
    PG_PLAYER,     /* one file, its pulses, and PLAY ONCE                */
    PG_RAW,        /* REC's level meter and ARM, one capture to a file   */
    PG_LEARN,      /* measure rate, deviation and sync                   */
    PG_SETTINGS,   /* every setting, as rows with their values           */
} sg_page_t;
static sg_page_t page;
static int home_sel, settings_sel;
static char hint_text[80];
/* What each button on the current page's bar does, set while drawing, so a
   tap and ENTER on a focused button act on what was on the screen. */
static char bar_keys[8];
static int bar_n;

static void go(sg_page_t p);

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
        if(saved_files>=SAVED_FILES_MAX){saved_truncated=true;break;}
        saved_file_t *f=&saved_file[saved_files];
        /* A path that does not fit could not be opened again; skip it
           rather than list a name that leads nowhere. */
        if(strlen(dir)+1+strlen(e->d_name)>=sizeof(f->path))continue;
        memcpy(f->path,dir,strlen(dir));f->path[strlen(dir)]='/';
        strcpy(f->path+strlen(dir)+1,e->d_name);
        snprintf(f->name,sizeof(f->name),"%.47s",e->d_name);
        struct stat st;
        if(stat(f->path,&st)==0){f->size=(uint32_t)st.st_size;f->mtime=(long)st.st_mtime;}
        else {f->size=0;f->mtime=0;}
        saved_files++;
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
#define PLAYER_EDGES 8192
static EXT_RAM_BSS_ATTR subghz_file_t player_sub;
static EXT_RAM_BSS_ATTR int32_t player_edges[PLAYER_EDGES];
static EXT_RAM_BSS_ATTR char player_line[4608];
static void saved_open(void)
{
    if(saved_sel<s.count){selected=saved_sel;action('c');return;}
    const int i=saved_sel-s.count;
    if(i<0 || i>=saved_files)return;
    if(!subghz_file_load(saved_file[i].path,&player_sub,player_edges,PLAYER_EDGES,
                         player_line,sizeof(player_line))) {
        snprintf(feedback,sizeof(feedback),"Could not read %s",saved_file[i].name);
        return;
    }
    if(!ls_rec_player_load(saved_file[i].path,&player_sub,player_edges)) {
        /* A custom-preset 2-FSK file - REC's own FSK saves, and the
           Flipper's - carries no rate or sync word for the SX1262 to rebuild
           a packet from. The Flipper sends it as it is. */
        const bool custom=strstr(player_sub.preset,"Custom")!=NULL;
        snprintf(feedback,sizeof(feedback),"%s",ls_rec_player_busy()?"A replay is still sending":
                 custom?"Custom FSK preset - send this one from the Flipper":
                 "Not a RAW file this board can send");
        return;
    }
    go(PG_PLAYER);
}

static void settings_open_row(int row);
static const ls_opt_ctx_t *subghz_options(void);

static void action(char c)
{
    feedback[0]=0;
    /* Say why, then stop. Every branch below also guards itself, so this is
       about telling the operator rather than about safety. */
    if(c!='d') {
        const char *no=disabled_reason(c);
        if(no){snprintf(feedback,sizeof(feedback),"%s",no);return;}
    }
    if(c=='b') {
        go(page==PG_PLAYER?PG_SAVED:PG_HOME);
    } else if(c=='w') {
        bool on=!rec_watch_enabled();
        if(rec_watch_enable(on)) {
            if(on && rec_watch_source()==REC_SOURCE_RTL){ls_tui_radio_want("REC");rec_arm_request();}
            else if(!on)rec_disarm();
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
        if(selected>=s.count){snprintf(feedback,sizeof(feedback),"Nothing captured yet");return;}
        const rec_watch_event_t *e=&s.event[selected];
        char title[40];
        snprintf(title,sizeof(title),"CAPTURE #%lu",(unsigned long)e->id);
        menu_n=0;
        ls_picker_open(title,capture_menu_done);
        /* Offered only when it can actually be done. A capture from a
           receiver that cannot transmit, or one with no modulation recorded,
           has nothing to send it under. */
        if(rec_source_can_replay((rec_source_t)e->source) && (e->source==REC_SOURCE_CC1101 || e->bitrate))
            menu_add('R',"REPLAY","Send it again - pick the power");
        menu_add('S',"SAVE .SUB","Flipper file on the card");
        menu_add('P',e->pinned?"UNPIN":"PIN",e->pinned?"Allow eviction":"Keep it");
        menu_add('J',"JOURNAL","Mark with GPS and time");
    } else if(c=='m') {
        go(PG_SETTINGS);
    } else if(c=='o') {
        ls_opt_open(subghz_options());
    } else if(c=='i') {notes=!notes;
    } else if(c=='n') {
        /* One press either way: a running sweep holds the radio, and letting
           go of it must never be a menu away. */
        if(page!=PG_ANALYZE)go(PG_ANALYZE);
        if(rec_watch_scan_busy()){rec_watch_scan_stop();snprintf(feedback,sizeof(feedback),"Sweep stopped");}
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
        go(PG_LEARN);
    } else if(c=='L') {
        learn_open();
    } else if(c=='A') {
        rec_learn_t r;
        if(!rec_watch_learn_result(&r)){snprintf(feedback,sizeof(feedback),"Nothing learned yet - LISTEN first");return;}
        if(!rec_watch_learn_apply()){snprintf(feedback,sizeof(feedback),"Stop WATCH first");return;}
        snprintf(feedback,sizeof(feedback),"Applied %lu baud, sync %08lX",
                 (unsigned long)r.bitrate,(unsigned long)r.sync_word);
    } else if(c=='O') {
        saved_open();
    } else if(c=='U') {
        saved_reload();
        snprintf(feedback,sizeof(feedback),"%d file%s on the card",saved_files,saved_files==1?"":"s");
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
        /* What actually made the recording, named from its source. */
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
    } else if(c=='d') {
        go(PG_RAW);
    } else if(c=='r') {
        source_open();
    } else if(c=='R') {
        ls_rsel_open(LS_RSEL_SUBGHZ_SWEEP,sweep_done);
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
    button_focus=-1;button_slot=0;
    if(p==PG_SAVED && page!=PG_PLAYER){saved_reload();saved_sel=0;}
    if(p==PG_RAW)ls_rec_tools_enter();
    if(p==PG_SETTINGS)settings_sel=0;
    page=p;
}

static void enter(void)
{
    button_focus=-1;button_slot=0;rec_watch_start();ls_field_start();ls_field_watch(true);
    /* The capture source is the truth for READ's RADIO button. */
    ls_rsel_track(LS_RSEL_SUBGHZ_READ,source_radio);
    feedback[0]=0;page=PG_HOME;
}
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
    snprintf(text,sizeof(text),"%s #%lu / %s",ls_rsel_name(rec_source_radio((rec_source_t)e->source)),(unsigned long)e->id,
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
        /* What it is, where that is known, rather than the payload alone. */
        char what[64];
        subghz_pwm_format(&s.pwm[selected], what, sizeof(what));
        if(!what[0]) subghz_nrz_format(&s.nrz[selected], what, sizeof(what));
        if(what[0]) snprintf(text,sizeof(text),"%s / %u frames / %u us unit",what,s.decoded[selected].repeats,s.decoded[selected].unit_us);
        else snprintf(text,sizeof(text),"OOK24 %06lX / %u frames / %u us unit",(unsigned long)s.decoded[selected].value,s.decoded[selected].repeats,s.decoded[selected].unit_us);
        tui_put_str(sf,a,a.x+2,a.y+9,text,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    }
}

/* The bar along the top of every page: BACK first, always in the same
   place, then that page's own controls. Sized by the shared rule - three rows
   in a wide pane when the labels fit the compact form, the fat form
   otherwise - so no page spends more on buttons than the old screen did. */
static void page_bar(tui_surface *sf,tui_rect *a,const ls_btn_t *btn,int n)
{
    const bool wide=a->w>a->h*2;
    const bool thumbs=!ls_tui_keyboard_mode() && a->h>=22;
    int h=wide?(!thumbs && ls_btn_compact_fits(tui_rect_make(a->x,a->y,a->w,3),btn,n)?3:4)
              :ls_btn_raised_height(*a,n);
    /* A tall narrow split: two columns of buttons, so it needs the rows. */
    if(a->w>=32 && a->w<38 && a->h>=36 && n>=5) h=16;
    if(h>a->h-6)h=a->h>9?3:0;
    if(h<=0)return;
    ls_btn_bar_raised(sf,tui_rect_make(a->x,a->y,a->w,h),btn,n,button_focus);
    bar_n=n<(int)sizeof(bar_keys)?n:(int)sizeof(bar_keys);
    for(int i=0;i<bar_n;i++)bar_keys[i]=btn[i].key;
    a->y+=h;a->h-=h;
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

/* ------------------------------------------------------------------ HOME */

#define HOME_TILES 6
static const char *const HOME_NAME[HOME_TILES]={"READ","ANALYZER","SAVED","READ RAW","LEARN","SETTINGS"};
static const sg_page_t HOME_PAGE[HOME_TILES]={PG_READ,PG_ANALYZE,PG_SAVED,PG_RAW,PG_LEARN,PG_SETTINGS};

static void draw_home(tui_surface *sf,tui_rect a,const rec_hub_status_t *rx,uint8_t fresh)
{
    banner(sf,&a,rx,fresh);
    static char sub[HOME_TILES][32];
    /* Twelve characters: three tiles across a portrait pane leave that. */
    snprintf(sub[0],sizeof(sub[0]),s.enabled?"ON, %d caught":"Catch signals",s.count);
    snprintf(sub[1],sizeof(sub[1]),"%s",rec_watch_scan_busy()?"Sweeping":"Find freq");
    snprintf(sub[2],sizeof(sub[2]),"Replay files");
    char rtl_only[16];
    snprintf(rtl_only,sizeof(rtl_only),"%s only",ls_rsel_name(LS_RSEL_SDR_RTL));
    snprintf(sub[3],sizeof(sub[3]),"%s",rec_watch_source()==REC_SOURCE_RTL?"One to file":rtl_only);
    snprintf(sub[4],sizeof(sub[4]),"%s",rec_watch_learn_busy()?"Listening":"Rate & sync");
    snprintf(sub[5],sizeof(sub[5]),"%.3f MHz",rec_get_freq()/1e6);
    const ls_tile_t tiles[HOME_TILES]={
        {HOME_NAME[0],sub[0],LS_ICON_WAVE,   TUI_GREEN, s.enabled},
        {HOME_NAME[1],sub[1],LS_ICON_FALLS,  TUI_BLUE,  rec_watch_scan_busy()},
        {HOME_NAME[2],sub[2],LS_ICON_FILES,  TUI_YELLOW,false},
        {HOME_NAME[3],sub[3],LS_ICON_RECORD, TUI_RED,   false},
        {HOME_NAME[4],sub[4],LS_ICON_LABS,   TUI_CYAN,  rec_watch_learn_busy()},
        {HOME_NAME[5],sub[5],LS_ICON_GEAR,   TUI_WHITE, false}};
    tui_rect grid=tui_rect_make(a.x,a.y+1,a.w,a.h-2);
    if(home_sel<0 || home_sel>=HOME_TILES)home_sel=0;
    ls_tile_grid(sf,grid,tiles,HOME_TILES,home_sel);
    bar_n=0;
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"Tap a tile or press 1-6",LS_ATTR_DIM);
}

static bool key_home(ls_tk_t k,char ch)
{
    int cols=1,rows=1;ls_tile_shape(&cols,&rows);if(cols<1)cols=1;
    switch(k) {
    case LS_TK_LEFT:  if(home_sel>0)home_sel--;return true;
    case LS_TK_RIGHT: if(home_sel+1<HOME_TILES)home_sel++;return true;
    case LS_TK_UP:    if(home_sel-cols>=0)home_sel-=cols;return true;
    case LS_TK_DOWN:  if(home_sel+cols<HOME_TILES)home_sel+=cols;return true;
    case LS_TK_ENTER: go(HOME_PAGE[home_sel]);return true;
    default: break;
    }
    if(k==LS_TK_CHAR && ch>='1' && ch<'1'+HOME_TILES){home_sel=ch-'1';go(HOME_PAGE[home_sel]);return true;}
    return false;
}

/* ------------------------------------------------------------------ READ */

static void draw_read(tui_surface *sf,tui_rect a,const rec_hub_status_t *rx,
                      const ls_mixrf_status_t *cc,uint8_t fresh)
{
    const bool cc_source=rec_watch_source()==REC_SOURCE_CC1101;
    char freq[16];snprintf(freq,sizeof(freq),"%.3f",rec_get_freq()/1e6);
    ls_btn_t btn[]={
        {"BACK","HOME",'b',false,false},
        {"FREQ",freq,'f',false,disabled_reason('f')!=NULL},
        {"WATCH",s.enabled?"ON":"OFF",'w',s.enabled,disabled_reason('w')!=NULL},
        ls_rsel_button(LS_RSEL_SUBGHZ_READ),
        {"OPEN",s.count?"CAPTURE":"NONE",'c',false,!s.count},
        ls_opt_button(subghz_options())};
    /* PREV/NEXT only on a pane too narrow to walk the list another way. */
    if(touch_nav) {
        ls_btn_t bottom[2]={
            {"PREV","PATTERN",',',false,!s.count || selected==0},
            {"NEXT","PATTERN",'.',false,selected+1>=s.count}};
        const int bh=5;
        if(a.h>bh+20) {
            ls_btn_bar_raised_slot(sf,tui_rect_make(a.x,a.y+a.h-bh,a.w,bh),bottom,2,-1,LS_BTN_SLOT_WATERFALL);
            a.h-=bh;
        }
    }
    btn[3].dim=s.enabled;
    banner(sf,&a,rx,fresh);
    page_bar(sf,&a,btn,6);
    tui_rect body=tui_rect_make(a.x,a.y,a.w,a.h-1);
    tui_rect content=tui_rect_make(body.x+1,body.y+1,body.w-2,body.h-2);
    char panel_title[48];
    snprintf(panel_title,sizeof(panel_title),"READ / %s %s",source_name(),
             rec_watch_source()==REC_SOURCE_SX1262?"2-FSK":"OOK");
    ls_panel_box(sf,body,panel_title,TUI_CYAN);
    ls_motion_busy(sf,body,s.exporting || (s.enabled && rx->receiver_streaming));
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
    int first=selected/rows*rows;
    if(!s.count)tui_put_str(sf,content,list.x,list.y,
                            s.enabled?"Listening - press the remote":"Nothing caught yet. Press WATCH.",LS_ATTR_DIM);
    for(int i=0;i<rows && first+i<s.count;i++) {
        const rec_watch_event_t *e=&s.event[first+i];int y=list.y+i*2;
        if(first+i==selected)ls_fill_dither(sf,tui_rect_make(list.x,y,list.w,2),LS_DITHER_LIGHT,TUI_CYAN);
        snprintf(line,sizeof(line),"%s #%lu %.4fMHz x%lu",e->last_boot==s.boot_id?"[RX]":"[SD]",(unsigned long)e->id,e->frequency/1e6,(unsigned long)e->count);
        tui_put_str(sf,list,list.x,y,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        char what[64];
        subghz_pwm_format(&s.pwm[first+i], what, sizeof(what));
        if(!what[0]) subghz_nrz_format(&s.nrz[first+i], what, sizeof(what));
        const char *heard=ls_rsel_name(rec_source_radio((rec_source_t)e->source));
        if(what[0])snprintf(line,sizeof(line),"%s %s / %u repeats",heard,what,s.decoded[first+i].repeats);
        else if(s.decoded[first+i].repeats)snprintf(line,sizeof(line),"%s OOK24 %06lX / %u repeats",heard,(unsigned long)s.decoded[first+i].value,s.decoded[first+i].repeats);
        else snprintf(line,sizeof(line),"%s %u edges %.1fms",e->end_reason!=REC_END_GAP?"LIMIT":e->count>1?"REPEAT":"RAW?",e->edges,e->span_us/1000.);
        tui_put_str(sf,list,list.x,y+1,line,LS_ATTR_DIM);
    }
    if(notes) {
        tui_rect info=tui_rect_make(body.x+1,body.y+1,body.w-2,body.h-2);
        tui_fill(sf,info,' ',LS_ATTR_DIM);
        ls_panel_box(sf,info,"WHAT THE LIST MEANS",TUI_CYAN);
        tui_put_str(sf,info,info.x+2,info.y+1,s.storage,LS_ATTR_DIM);
        snprintf(line,sizeof(line),"Loss: %lu skipped / CC buffer overflows %lu",(unsigned long)s.dropped,(unsigned long)cc->raw_overflows);
        tui_put_str(sf,info,info.x+2,info.y+2,line,LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+3,"RAW? unverified / REPEAT timing match",LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+4,"[SD] earlier boot / [RX] this boot",LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+5,"Pattern match is not a device identity.",LS_ATTR_DIM);
        if(cc_source) snprintf(line,sizeof(line),"CC: 650kHz OOK / 40us filter / 30ms gap");
        else if(rec_watch_source()==REC_SOURCE_SX1262) snprintf(line,sizeof(line),"%s: 2-FSK, rate and deviation in SETTINGS",source_name());
        else snprintf(line,sizeof(line),"%s: configurable OOK pulse capture",source_name());
        tui_put_str(sf,info,info.x+2,info.y+6,line,LS_ATTR_DIM);
        snprintf(line,sizeof(line),"DM queued %lu / refused %lu / limited %lu",(unsigned long)s.alert_sent,(unsigned long)s.alert_failed,(unsigned long)s.alert_suppressed);
        tui_put_str(sf,info,info.x+2,info.y+7,line,LS_ATTR_DIM);
    }
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:s.export_status[0]?s.export_status:
                 "UP/DOWN pick a capture, ENTER opens it",LS_ATTR_DIM);
}

/* -------------------------------------------------------------- ANALYZER */

static void draw_analyze(tui_surface *sf,tui_rect a,const rec_hub_status_t *rx,uint8_t fresh)
{
    const bool sweeping=rec_watch_scan_busy();
    const int found=rec_watch_scan_result(NULL,0);
    char thr[16];snprintf(thr,sizeof(thr),"%.0f dB",(double)rec_watch_scan_threshold());
    ls_btn_t btn[]={
        {"BACK","HOME",'b',false,false},
        {"SCAN",sweeping?"STOP":"START",'n',sweeping,disabled_reason('n')!=NULL},
        {"BAND","PICK",'W',false,rec_watch_enabled()},
        {"DETECT",thr,'T',false,false},
        {"VIEW",sweep_hits?"BAND":"FOUND",'V',sweep_hits,!sweeping && found<=0},
        ls_rsel_button(LS_RSEL_SUBGHZ_SWEEP),
        ls_opt_button(subghz_options())};
    /* Its own letter on this bar, so R opens the sweep's list here and the
       capture source's everywhere else. */
    btn[5].key='R';
    banner(sf,&a,rx,fresh);
    page_bar(sf,&a,btn,7);
    tui_rect body=tui_rect_make(a.x,a.y,a.w,a.h-1);
    char title[40];
    if(sweep_hits) snprintf(title,sizeof(title),"ANALYZER / DETECTIONS");
    else snprintf(title,sizeof(title),"ANALYZER / %s SWEEP",ls_rsel_name(LS_RSEL_LORA));
    ls_panel_box(sf,body,title,TUI_CYAN);
    if(sweeping)hit_flash_poll();
    if(sweep_hits || (!sweeping && found>0)) {
        hits_table(sf,tui_rect_make(body.x+2,body.y+1,body.w-4,body.h-2));
        ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"Tap a detection to capture, tune or zoom",LS_ATTR_DIM);
        return;
    }
    if(sweeping) {
        spectrum(sf,tui_rect_make(body.x+2,body.y+1,body.w-4,body.h-2));
        ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"Transmit now - a column jumps where it is",LS_ATTR_DIM);
        return;
    }
    tui_rect w=tui_rect_make(body.x+2,body.y+2,body.w-4,body.h-4);
    tui_put_str(sf,w,w.x,w.y,"Find a transmitter you do not know the",TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
    tui_put_str(sf,w,w.x,w.y+1,"frequency of:",TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
    tui_put_str(sf,w,w.x,w.y+3,"1. SCAN, and pick a band",LS_ATTR_DIM);
    tui_put_str(sf,w,w.x,w.y+4,"2. Press the transmitter a few times",LS_ATTR_DIM);
    tui_put_str(sf,w,w.x,w.y+5,"3. Tap what it found: capture, tune, zoom",LS_ATTR_DIM);
    char where[64];
    snprintf(where,sizeof(where),"Sweeps on the %s; mesh pauses meanwhile.",ls_rsel_name(LS_RSEL_LORA));
    tui_put_str(sf,w,w.x,w.y+7,where,LS_ATTR_DIM);
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"SCAN starts a sweep",LS_ATTR_DIM);
}

/* ----------------------------------------------------------------- SAVED */

static void draw_saved(tui_surface *sf,tui_rect a)
{
    const int total=saved_total();
    if(saved_sel>=total)saved_sel=total?total-1:0;
    const bool on_file=saved_sel>=s.count;
    ls_btn_t btn[]={
        {"BACK","HOME",'b',false,false},
        {"OPEN",!total?"NONE":on_file?"PLAYER":"CAPTURE",'O',false,!total},
        {"RELOAD","CARD",'U',false,false}};
    page_bar(sf,&a,btn,3);
    tui_rect body=tui_rect_make(a.x,a.y,a.w,a.h-1);
    ls_panel_box(sf,body,"SAVED",TUI_YELLOW);
    saved_list=tui_rect_make(body.x+2,body.y+1,body.w-4,body.h-2);
    char line[112];
    if(!total) {
        tui_put_str(sf,saved_list,saved_list.x,saved_list.y,"Nothing yet.",TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        tui_put_str(sf,saved_list,saved_list.x,saved_list.y+2,"Captures from READ and READ RAW, and",LS_ATTR_DIM);
        tui_put_str(sf,saved_list,saved_list.x,saved_list.y+3,".sub files on the card, show up here.",LS_ATTR_DIM);
        ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"",LS_ATTR_DIM);
        return;
    }
    /* Two lines a row, a heading where the kind changes. */
    const int rows=saved_list.h/2>0?saved_list.h/2:1;
    if(saved_sel<saved_first)saved_first=saved_sel;
    if(saved_sel>=saved_first+rows-1)saved_first=saved_sel-rows+2;
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
            const rec_watch_event_t *e=&s.event[i];
            snprintf(line,sizeof(line),"#%lu  %.4f MHz  x%lu%s",(unsigned long)e->id,e->frequency/1e6,
                     (unsigned long)e->count,e->pinned?"  PINNED":"");
            tui_put_str(sf,saved_list,saved_list.x,y,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
            snprintf(line,sizeof(line),"%s  %u edges  %.1f ms",ls_rsel_name(rec_source_radio((rec_source_t)e->source)),
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
                 on_file?"ENTER opens it in the player":"ENTER: replay, save .sub, pin, journal",LS_ATTR_DIM);
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
    ls_btn_t btn[]={{"BACK","SAVED",'b',false,false}};
    page_bar(sf,&a,btn,1);
    ls_rec_player_draw(sf,a);
}

/* -------------------------------------------------------------- READ RAW */

static void draw_raw(tui_surface *sf,tui_rect a)
{
    ls_btn_t btn[]={{"BACK","HOME",'b',false,false}};
    page_bar(sf,&a,btn,1);
    const char *no=disabled_reason('d');
    if(no) {
        char how[48];
        snprintf(how,sizeof(how),"RADIO in READ picks the %s",ls_rsel_name(LS_RSEL_SDR_RTL));
        ls_panel_notice(sf,a,"READ RAW",no,how);
        return;
    }
    ls_rec_tools_draw(sf,a);
}

/* ----------------------------------------------------------------- LEARN */

static void draw_learn(tui_surface *sf,tui_rect a,const rec_hub_status_t *rx,uint8_t fresh)
{
    rec_learn_t r;
    const bool have=rec_watch_learn_result(&r);
    const bool learning=rec_watch_learn_busy();
    ls_btn_t btn[]={
        {"BACK","HOME",'b',false,false},
        {"LISTEN",learning?"BUSY":"30 S",'L',learning,learning},
        {"APPLY",have?"THESE":"NONE",'A',false,!have || learning},
        ls_opt_button(subghz_options())};
    banner(sf,&a,rx,fresh);
    page_bar(sf,&a,btn,4);
    tui_rect body=tui_rect_make(a.x,a.y,a.w,a.h-1);
    ls_panel_box(sf,body,"LEARN SIGNAL",TUI_CYAN);
    tui_rect w=tui_rect_make(body.x+2,body.y+2,body.w-4,body.h-4);
    if(learning) {
        char head[72];
        snprintf(head,sizeof(head),"%s",rec_watch_scan_stage());
        tui_put_str(sf,w,w.x,w.y,head[0]?head:"Working",TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        const int pct=rec_watch_scan_progress();
        char pctxt[8];snprintf(pctxt,sizeof(pctxt),"%d%%",pct);
        if((int)strlen(pctxt)+2<w.w)
            tui_put_str(sf,w,w.x+w.w-(int)strlen(pctxt),w.y,pctxt,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        if(w.w>8 && w.h>3) {
            const int full=(w.w-2)*pct/100;
            for(int i=0;i<w.w-2;i++)
                tui_put_char(sf,w,w.x+1+i,w.y+2,i<full?LS_TUI_SHADE_75:LS_TUI_SHADE_25,
                             TUI_ATTR((i<full?TUI_GREEN:TUI_CYAN)|TUI_BRIGHT,TUI_BLACK));
        }
        if(w.h>5) tui_put_str(sf,w,w.x,w.y+4,"Keep pressing the transmitter",LS_ATTR_DIM);
    } else if(have) {
        char v[80];
        tui_put_str(sf,w,w.x,w.y,"WHAT IT HEARD",TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
        snprintf(v,sizeof(v),"Bit rate    %lu baud",(unsigned long)r.bitrate);
        tui_put_str(sf,w,w.x,w.y+2,v,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        snprintf(v,sizeof(v),"Deviation   %.1f kHz",r.deviation_hz/1000.0);
        tui_put_str(sf,w,w.x,w.y+3,v,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        snprintf(v,sizeof(v),"Sync word   %08lX",(unsigned long)r.sync_word);
        tui_put_str(sf,w,w.x,w.y+4,v,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        snprintf(v,sizeof(v),"Confidence  %u%%",r.confidence);
        tui_put_str(sf,w,w.x,w.y+5,v,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        tui_put_str(sf,w,w.x,w.y+6,r.note,LS_ATTR_DIM);
        snprintf(v,sizeof(v),"APPLY sets the %s to receive it.",ls_rsel_name(LS_RSEL_LORA));
        tui_put_str(sf,w,w.x,w.y+8,v,LS_ATTR_DIM);
    } else {
        tui_put_str(sf,w,w.x,w.y,"Work out how a transmitter talks:",TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        tui_put_str(sf,w,w.x,w.y+2,"1. Tune to it (SETTINGS > Frequency)",LS_ATTR_DIM);
        tui_put_str(sf,w,w.x,w.y+3,"2. LISTEN, and keep pressing it",LS_ATTR_DIM);
        tui_put_str(sf,w,w.x,w.y+4,"3. APPLY what it measured",LS_ATTR_DIM);
    }
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"",LS_ATTR_DIM);
}

/* -------------------------------------------------------------- SETTINGS */

/* Every setting this app has, with its value beside it. Each row opens the
   same list or keypad it always did; what changed is that they are all here
   instead of spread over a bar, a MORE menu and two sub-menus. */
#define SETTING_ROWS 9
static tui_rect settings_list;
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
        else snprintf(out,len,"gap %d ms, min %d edges",rec_get_gap_ms(),rec_get_min_edges());
        break;
    case 3: snprintf(out,len,"%.0f dB over floor",(double)rec_watch_scan_threshold()); break;
    case 4: snprintf(out,len,"%s",ON_HIT_NAME[(int)rec_watch_scan_on_hit_get()]); break;
    case 5: snprintf(out,len,"%s",s.alerts?"mesh DM on":"off"); break;
    case 6: scan_look_load();snprintf(out,len,"%s",SCAN_GRAIN_NAME[scan_grain]); break;
    case 7: scan_look_load();snprintf(out,len,"%s",SCAN_LOOK_NAME[scan_look]); break;
    default: snprintf(out,len,"%s",notes?"shown":"hidden"); break;
    }
}
static void settings_open_row(int row)
{
    if(row<0 || row>=SETTING_ROWS)return;
    action(SETTING_KEY[row]);
}

/* OPTIONS: the SETTINGS rows that belong to the page in front of you, each
   doing what its row on SETTINGS does, and greyed for the same reasons. arg
   is the SETTINGS row. */
static void o_row(const ls_opt_t *o) { action(SETTING_KEY[o->arg]); }
static void o_row_show(const ls_opt_t *o,char *out,size_t n) { settings_value(o->arg,out,n); }
static const char *o_row_why(const ls_opt_t *o) { return disabled_reason(SETTING_KEY[o->arg]); }
#define SG_ROW(name,row) { .label=(name), .kind=LS_OPT_ACTION, .arg=(row), .act=o_row, \
                           .show=o_row_show, .why_not=o_row_why }
static const ls_opt_t OPT_READ[]={
    SG_ROW("Frequency",0),SG_ROW("Capture setup",2),SG_ROW("Mesh alerts",5),SG_ROW("List legend",8)};
static const ls_opt_t OPT_ANALYZE[]={
    SG_ROW("Detect threshold",3),SG_ROW("On detection",4),SG_ROW("Spectrum style",6),SG_ROW("Spectrum colour",7)};
static const ls_opt_t OPT_LEARN[]={SG_ROW("Frequency",0)};
#undef SG_ROW
static const ls_opt_ctx_t CTX_READ={.name="READ",.job=LS_RSEL_SUBGHZ_READ,.radio=LS_RSEL_NONE,LS_OPT_ROWS(OPT_READ)};
static const ls_opt_ctx_t CTX_ANALYZE={.name="ANALYZER",.job=LS_RSEL_SUBGHZ_SWEEP,.radio=LS_RSEL_NONE,LS_OPT_ROWS(OPT_ANALYZE),.tag="SWEEP"};
static const ls_opt_ctx_t CTX_LEARN={.name="LEARN",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(OPT_LEARN)};
/* SETTINGS is every row already, and the other pages have nothing to set. */
static const ls_opt_ctx_t *subghz_options(void)
{
    switch(page) {
    case PG_READ:    return &CTX_READ;
    case PG_ANALYZE: return &CTX_ANALYZE;
    case PG_LEARN:   return &CTX_LEARN;
    default:         return NULL;
    }
}
static void draw_settings(tui_surface *sf,tui_rect a)
{
    static const char *const LABEL[SETTING_ROWS]={
        "Frequency","Radio","Capture setup","Detect threshold","On detection",
        "Mesh alerts","Spectrum style","Spectrum colour","List legend"};
    ls_btn_t btn[]={{"BACK","HOME",'b',false,false}};
    page_bar(sf,&a,btn,1);
    tui_rect body=tui_rect_make(a.x,a.y,a.w,a.h-1);
    ls_panel_box(sf,body,"SETTINGS",TUI_WHITE);
    settings_list=tui_rect_make(body.x+2,body.y+1,body.w-4,body.h-2);
    /* Two lines a row where there is room - label over value - and one,
       value at the right, where there is not. */
    const int per=settings_list.h>=SETTING_ROWS*2?2:1;
    if(settings_sel<0 || settings_sel>=SETTING_ROWS)settings_sel=0;
    char v[48];
    for(int i=0;i<SETTING_ROWS;i++) {
        const int y=settings_list.y+i*per;
        if(y+per>settings_list.y+settings_list.h)break;
        if(i==settings_sel)ls_fill_dither(sf,tui_rect_make(settings_list.x,y,settings_list.w,per),LS_DITHER_LIGHT,TUI_CYAN);
        settings_value(i,v,sizeof(v));
        tui_put_str(sf,settings_list,settings_list.x,y,LABEL[i],TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        if(per==2) tui_put_str(sf,settings_list,settings_list.x+2,y+1,v,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        else {
            const int x=settings_list.x+settings_list.w-(int)strlen(v);
            tui_put_str(sf,settings_list,x>settings_list.x+18?x:settings_list.x+18,y,v,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
        }
    }
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"UP/DOWN pick, ENTER changes it",LS_ATTR_DIM);
}

/* ------------------------------------------------------------------ draw */

static void draw(tui_surface *sf,tui_rect a)
{
    if(a.w<24 || a.h<17) {ls_panel_notice(sf,a,"SUB-GHZ","Enlarge the pane","WATCH keeps its state");return;}
    rec_watch_snapshot(&s);
    uint8_t fresh=ls_fresh(&arrivals,s.received,600);
    touch_nav=a.w<90 && a.h>35;
    if(selected>=s.count)selected=s.count?s.count-1:0;
    if (!strncmp(feedback,"Sending #",9) && strcmp(s.export_status,"Replay queued") &&
        (!strncmp(s.export_status,"Sent #",6) || !strncmp(s.export_status,"Replay",6)))
        snprintf(feedback,sizeof(feedback),"%s",s.export_status);
    rec_hub_status_t rx;rec_get_hub_status(&rx);
    ls_mixrf_status_t cc;ls_mixrf_snapshot(&cc);
    if(rec_watch_source()==REC_SOURCE_CC1101){rx.freq_hz=rec_get_freq();rx.receiver_streaming=cc.capturing && cc.receiving;}
    /* The SX1262 is not on the RTL's hub. WATCH only turns on once its
       receive session has started, so on this source enabled is listening. */
    if(rec_watch_source()==REC_SOURCE_SX1262){rx.freq_hz=rec_get_freq();rx.receiver_streaming=s.enabled;}
    bar_n=0;det_hit=tui_rect_make(0,0,0,0);det_row_n=0;
    /* The threshold strip keeps its place while the spectrum is up, so a
       drag can follow the finger; anywhere else it is not on the screen. */
    if(page!=PG_ANALYZE || !rec_watch_scan_busy())gate_hit=tui_rect_make(0,0,0,0);
    static const char *const HINT[]={
        "1-6 open  ESC leaves SUB-GHZ",
        "W watch  F freq  R radio  O options  ENTER open  ESC back",
        "N scan/stop  R radio  O options  ESC back",
        "ENTER open  ESC back",
        "P play  +/- power  ESC back",
        "ENTER arm/stop  ESC back",
        "O options  ESC back",
        "ENTER change  ESC back"};
    snprintf(hint_text,sizeof(hint_text),"%s",HINT[page]);
    switch(page) {
    case PG_HOME:     draw_home(sf,a,&rx,fresh); break;
    case PG_READ:     draw_read(sf,a,&rx,&cc,fresh); break;
    case PG_ANALYZE:  draw_analyze(sf,a,&rx,fresh); break;
    case PG_SAVED:    draw_saved(sf,a); break;
    case PG_PLAYER:   draw_player(sf,a); break;
    case PG_RAW:      draw_raw(sf,a); break;
    case PG_LEARN:    draw_learn(sf,a,&rx,fresh); break;
    case PG_SETTINGS: draw_settings(sf,a); break;
    }
}

/* ------------------------------------------------------------ keys, taps */

static bool back_key(ls_tk_t k,char ch)
{
    return k==LS_TK_ESC || k==LS_TK_BACKSPACE || (k==LS_TK_CHAR && (ch=='b' || ch=='B'));
}

static bool key(ls_tk_t k,char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if(page==PG_HOME) {
        if(key_home(k,ch))return true;
    } else if(back_key(k,ch)) {
        go(page==PG_PLAYER?PG_SAVED:PG_HOME);
        return true;
    }
    /* A focused button on the bar first, so ENTER on BACK goes back. LEFT
       and RIGHT walk the bar, except in the player, where they walk the
       pulses. */
    if(page!=PG_HOME && (k==LS_TK_TAB ||
       (page!=PG_PLAYER && (k==LS_TK_LEFT || k==LS_TK_RIGHT))))
        return ls_btn_navigate(k,&button_slot,&button_focus,false);
    if(k==LS_TK_ENTER && button_focus>=0 && button_focus<bar_n) {
        action(bar_keys[button_focus]);
        return true;
    }
    /* Then the page's own keys, before the shortcuts every page shares. */
    if(page==PG_PLAYER) {
        const int r=ls_rec_player_key(k,ch);
        if(r==2){go(PG_SAVED);return true;}
        if(r==1)return true;
    }
    if(page==PG_RAW && !disabled_reason('d') && ls_rec_tools_key(k,ch))return true;
    if(k==LS_TK_UP || k==LS_TK_DOWN)button_focus=-1;
    switch(page) {
    case PG_READ:
        if(k==LS_TK_UP){if(selected)selected--;return true;}
        if(k==LS_TK_DOWN){if(selected+1<s.count)selected++;return true;}
        if(k==LS_TK_ENTER){action('c');return true;}
        break;
    case PG_SAVED:
        if(k==LS_TK_UP){if(saved_sel)saved_sel--;return true;}
        if(k==LS_TK_DOWN){if(saved_sel+1<saved_total())saved_sel++;return true;}
        if(k==LS_TK_ENTER){action('O');return true;}
        break;
    case PG_SETTINGS:
        if(k==LS_TK_UP){if(settings_sel)settings_sel--;return true;}
        if(k==LS_TK_DOWN){if(settings_sel+1<SETTING_ROWS)settings_sel++;return true;}
        if(k==LS_TK_ENTER){settings_open_row(settings_sel);return true;}
        break;
    case PG_ANALYZE:
        if(k==LS_TK_ENTER){action('n');return true;}
        break;
    case PG_LEARN:
        if(k==LS_TK_ENTER){action('L');return true;}
        break;
    default: break;
    }
    if(k!=LS_TK_CHAR || !ch)return false;
    /* The letter printed on a button of this page's bar wins over the shared
       shortcut with the same letter - on LEARN, A is APPLY, not ALERTS. */
    for(int i=0;i<bar_n;i++) {
        char b=bar_keys[i];
        if(b>='A' && b<='Z')b+='a'-'A';
        if(b==ch){action(bar_keys[i]);return true;}
    }
    if(page==PG_READ) {
        if(ch==','){if(selected)selected--;return true;}
        if(ch=='.'){if(selected+1<s.count)selected++;return true;}
    }
    /* Every action keeps its letter on every page - a shortcut that stopped
       working because its button moved would be a worse trade than the
       crowding it fixed. */
    if(!strchr("fswncmpejadirl",ch))return false;
    action(ch);return true;
}

static bool touch(int x,int y)
{
    if(page==PG_HOME) {
        const int t=ls_tile_hit(x,y);
        if(t>=0 && t<HOME_TILES){home_sel=t;go(HOME_PAGE[t]);}
        return true;
    }
    {
        const int i=ls_btn_hit(x,y);
        if(i>=0 && i<bar_n){action(bar_keys[i]);return true;}
    }
    switch(page) {
    case PG_PLAYER: {
        const int r=ls_rec_player_touch(x,y);
        if(r==2)go(PG_SAVED);
        return true;
    }
    case PG_RAW:
        if(!disabled_reason('d'))return ls_rec_tools_touch(x,y);
        return true;
    case PG_SETTINGS:
        if(tui_rect_contains(settings_list,x,y)) {
            const int per=settings_list.h>=SETTING_ROWS*2?2:1;
            const int row=(y-settings_list.y)/per;
            if(row>=0 && row<SETTING_ROWS){settings_sel=row;settings_open_row(row);}
        }
        return true;
    case PG_SAVED:
        if(tui_rect_contains(saved_list,x,y)) {
            const int i=saved_hit(y);
            if(i>=0){
                /* First tap selects, a tap on the selected entry opens it. */
                if(i==saved_sel)action('O');
                else saved_sel=i;
            }
        }
        return true;
    case PG_ANALYZE:
        if(det_hit.w>0 && tui_rect_contains(det_hit,x,y)) {
            sweep_hits=!sweep_hits;feedback[0]=0;return true;
        }
        if(det_row_n && tui_rect_contains(det_rows,x,y)) {
            const int i=y-det_rows.y;
            if(i>=0 && i<det_row_n) detection_open(det_row_hz[i]);
            return true;
        }
        if(rec_watch_scan_busy() && gate_hit.h>0 && tui_rect_contains(gate_hit,x,y)) {
            gate_set_from_row(y);
            snprintf(feedback,sizeof(feedback),"Detect above %.0f dB over the floor",
                     (double)rec_watch_scan_threshold());
        }
        return true;
    case PG_READ: {
        const int i=ls_btn_hit_slot(x,y,LS_BTN_SLOT_WATERFALL);
        if(i==0){if(selected)selected--;return true;}
        if(i==1){if(selected+1<s.count)selected++;return true;}
        if(!notes && pulse_total && tui_rect_contains(pulse_hit,x,y)) {
            uint64_t t=(uint64_t)(x-pulse_hit.x)*pulse_total/pulse_hit.w,sum=0;
            int n=s.event[selected].edges;if(n>48)n=48;
            for(int j=0;j<n;j++) {sum+=s.preview[selected][j]<0?-(int64_t)s.preview[selected][j]:s.preview[selected][j];if(sum>t){pulse_selected=j;break;}}
            return true;
        }
        if(x>=list.x && x<list.x+list.w && y>=list.y && y<list.y+list.h) {
            int rows=list.h/2;if(rows<1)rows=1;
            int n=selected/rows*rows+(y-list.y)/2;
            /* First tap selects, a tap on the selected capture opens it. */
            if(n<s.count){if(n==selected)action('c');else selected=n;}
        }
        return true;
    }
    default: return true;
    }
}
const ls_tui_screen_t ls_scr_subghz={.radio="REC",.name="SUB-GHZ",.hint=hint_text,.enter=enter,.leave=leave,.draw=draw,.key=key,.touch=touch};
