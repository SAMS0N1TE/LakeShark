/* @NAME@: @SUB@.
 *
 * Made by `lsdk new` from the service template (API 2.1): a small tick that
 * keeps running after the screen closes. It reads the board's decoded
 * timeline (pages, APRS, AIS, ADS-B, mesh) and posts a notice when a decode
 * contains the word you are watching for. The screen shows what the tick
 * has done; it changes only when the tick saw something.
 *
 * Keys: s starts or stops the watch, o opens OPTIONS.
 * The owner can stop it in RADIOS (APP SERVICE) or with `app stop @ID@`. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include <stdio.h>
#include <string.h>

LAP_ICON_PX("....................",
            "....................",
            "......bbbbbbbb......",
            "....bb........bb....",
            "...b....3333....b...",
            "...b...3....3...b...",
            "...b...3.ee.3...b...",
            "...b...3....3...b...",
            "...b....3333....b...",
            "....bb........bb....",
            "......bbbbbbbb......",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................");

static void tick(void);
static void stopped(void);
static lap_service_t svc = { tick, stopped, 250 };   /* every 250 ms */

static struct {
    bool ready;
    uint32_t seen, matched, ticks;
    char word[16];
    char last[72];              /* the last match, for the screen */
    int history[64];            /* decodes per tick, newest last */
    lap_event_t ev;             /* app memory, not the stack */
    lap_btn_t keys[2];
    char state[12];
} s;

/* ---- the tick: no drawing, back within 4 ms ------------------------------ */

static void tick(void)
{
    int n = 0;
    s.ticks++;
    while (n < 8 && lap_event_next(&s.ev) == 1) {      /* a few a tick, never a loop without end */
        n++;
        s.seen++;
        if (s.word[0] && strstr(s.ev.text, s.word)) {
            s.matched++;
            snprintf(s.last, sizeof(s.last), "%s: %.40s", s.ev.from, s.ev.text);
            lap_notify("@NAME@", s.last);
        }
    }
    memmove(s.history, s.history + 1, sizeof(s.history) - sizeof(s.history[0]));
    s.history[63] = n;
}

static void stopped(void) { lap_log("service stopped after %lu ticks", (unsigned long)s.ticks); }

static void start(void)
{
    lap_event_subscribe(LAP_EV_PAGE | LAP_EV_APRS | LAP_EV_AIS | LAP_EV_ADSB | LAP_EV_MESH);
    if (lap_service_start(&svc) != LAP_SVC_OK) lap_notify("@NAME@", "could not start the service");
}

static void stop(void)
{
    lap_service_stop();
    lap_event_subscribe(0);
}

/* ---- OPTIONS: the word to watch for -------------------------------------- */

static const char *const WORDS[] = { "TEST", "FIRE", "HELP", "WX" };
static int word_index(void)
{
    for (int i = 0; i < 4; i++) if (!strcmp(s.word, WORDS[i])) return i;
    return 0;
}
static double opt_get(int id) { (void)id; return word_index(); }
static void opt_set(int id, double v) { (void)id; snprintf(s.word, sizeof(s.word), "%s", WORDS[(int)v & 3]); }
static const lap_opt_t OPT_ROWS[] = {
    { .label = "WATCH FOR", .kind = LAP_OPT_CYCLE, .id = 0, .names = WORDS, .n = 4 },
};
static const lap_opt_list_t OPTIONS = { "@NAME@", OPT_ROWS, 1, opt_get, opt_set };

/* ---- the screen ------------------------------------------------------------ */

static void draw(void)
{
    const int w = lap_cols(), h = lap_rows();
    const bool on = lap_service_running();
    lap_clear(LAP_UI_DIM);
    lap_put_str(1, 1, "( ( o ) )", LAP_ATTR(on ? LAP_UI_LIVE : LAP_BLUE, LAP_BLACK));
    lap_put_str(12, 1, "@NAME@", LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK));
    lap_put_str(12, 2, on ? "watching in the background" : "stopped", on ? LAP_UI_TEXT : LAP_UI_DIM);
    lap_hline(0, 4, w, '-', LAP_UI_FAINT);

    char line[64];
    snprintf(line, sizeof(line), "watching for  %s", s.word);
    lap_put_str(1, 5, line, LAP_UI_TEXT);
    snprintf(line, sizeof(line), "decodes seen  %lu", (unsigned long)s.seen);
    lap_put_str(1, 6, line, LAP_ATTR(LAP_UI_LIVE, LAP_BLACK));
    snprintf(line, sizeof(line), "matches       %lu", (unsigned long)s.matched);
    lap_put_str(1, 7, line, LAP_ATTR(s.matched ? LAP_UI_WARN : LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 8, s.last[0] ? s.last : "no match yet", s.last[0] ? LAP_UI_TEXT : LAP_UI_DIM);
    if (!s.seen)                        /* say why it is quiet, rather than look broken */
        lap_put_str(1, 9, "nothing decoded yet: decodes come while FM, APRS, AIS, ADS-B or MESH run",
                    LAP_ATTR(LAP_CYAN, LAP_BLACK));

    const int kh = lap_ui_keys_rows(2), ky = h - kh;
    lap_box(0, 11, w, ky - 11 < 10 ? ky - 11 : 10, "DECODES A TICK", LAP_ATTR(LAP_UI_FRAME, LAP_BLACK));
    const int sh = (ky - 11 < 10 ? ky - 11 : 10) - 2;
    if (sh > 0) lap_ui_sparkline(1, 12, w - 2, sh, s.history, 64, 8, -1);

    snprintf(s.state, sizeof(s.state), "%s", on ? "on" : "off");
    s.keys[0] = (lap_btn_t){ on ? "STOP" : "START", s.state, 's', on, false, false };
    s.keys[1] = (lap_btn_t)LAP_BTN_OPTIONS;
    lap_ui_keys(0, ky, w, kh, s.keys, 2, -1);
}

static bool act(char ch)
{
    switch (ch | 0x20) {
    case 's': if (lap_service_running()) stop(); else start(); return true;
    case 'o': return lap_options_open(&OPTIONS);
    default: return false;
    }
}
static bool key(lap_key_t k, char ch) { return k == LAP_KEY_CHAR && act(ch); }
static bool touch(int col, int row)
{
    const int k = lap_ui_key_hit(col, row);
    return k == 0 ? act('s') : k == 1 ? act('o') : false;
}

static void enter(void)
{
    if (s.ready) return;                /* back from the background: all still here */
    memset(&s, 0, sizeof(s));
    snprintf(s.word, sizeof(s.word), "TEST");
    s.ready = true;
    start();                            /* watching from the first second; STOP (s) ends it */
}

static void leave(void)
{
    if (lap_service_running()) return;  /* the tick keeps the subscription */
    lap_event_subscribe(0);
}

LAP_APP(.id = "@ID@", .name = "@NAME@", .sub = "@SUB@", .version = "0.1",
        .author = "@AUTHOR@", .color = LAP_BLUE, .flags = LAP_CAP_RADIO_RX | LAP_CAP_SERVICE,
        .icon_art = { "  .----.  ", " / .--. \\ ", "| ( () ) |", " \\ '--' / ", "  '----'  ", "@ART@" },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
