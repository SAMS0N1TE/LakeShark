/* @NAME@: @SUB@.
 *
 * Made by `lsdk new` from the radio template (API 2.1): the board's own
 * receiver face on any radio with a spectrum (RTL-SDR, HackRF, LR2021; SIM
 * on the PC). Big rolling figures, the shared waterfall, the RADIO list, the
 * TUNE keypad and the layered OPTIONS list, all the board's own controls.
 * The screen moves only when a new spectrum line arrives.
 *
 * A board without a radio for a spectrum (a Nano with no SDR plugged in)
 * gets a page saying so and what to plug in; the app opens the radio by
 * itself when one arrives. In the simulator the radio is SIM: a carrier
 * and a Morse-keyed one, so there is something to see at once.
 *
 * Keys: r picks the radio, t tunes, o opens OPTIONS. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include <stdio.h>
#include <string.h>

LAP_ICON_PX("....................",
            ".........a..........",
            "........a.a.........",
            ".......a...a........",
            "......a.....a.......",
            "....aa.......aa.....",
            "aaaa...........aaaa.",
            "....................",
            "4444444444444444444.",
            "6.c.6.c.6.c.6.c.6.c.",
            ".c.6.c.6.c.6.c.6.c..",
            "c.6.c.6.c.6.c.6.c.6.",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................");

#define BINS 128

static struct {
    lap_radio_info_t radios[LAP_RADIOS_MAX];
    int nradios, radio;         /* radio: index in the list, -1 for the first that works */
    int h;                      /* the receive session, 0 when none */
    uint64_t hz;
    float db[BINS];
    uint32_t seq;
    int lines;                  /* spectrum lines seen: the screen's only motion */
    float peak_db;
    int tries;                  /* looks at the radio list since the app opened */
    bool sim;                   /* the simulator's SIM radio */
    char radio_name[16], status[64];
    lap_btn_t keys[3];
} s;

/* ---- the receiver ------------------------------------------------------- */

static void close_rx(void)
{
    if (s.h > 0) lap_rx_close(s.h);
    s.h = 0;
}

static const char *rx_error(int e)
{
    switch (e) {
    case LAP_RX_EACCES: return "radio access was refused: open the app again and allow RADIO";
    case LAP_RX_EBUSY: return "another receiver holds it: park it in RADIOS";
    case LAP_RX_ENODEV: return "it went away (unplugged?)";
    case LAP_RX_ERANGE: return "it cannot tune there";
    default: return "it would not open";
    }
}

static void open_rx(void)
{
    close_rx();
    s.tries++;
    s.nradios = lap_radio_list(s.radios, LAP_RADIOS_MAX);
    if (s.nradios < 0) s.nradios = 0;
    for (int i = 0; i < s.nradios; i++) {
        if (s.radio >= 0 && i != s.radio) continue;
        const lap_radio_info_t *r = &s.radios[i];
        if (!r->present || r->busy || !(r->features & LAP_RXF_SPECTRUM)) continue;
        if (s.hz < r->min_hz || s.hz > r->max_hz) s.hz = r->min_hz + (r->max_hz - r->min_hz) / 2;
        const lap_rx_config_t cfg = { s.hz, 0, LAP_GAIN_AUTO, LAP_RX_SPECTRUM_ONLY };
        s.h = lap_rx_open(i, &cfg);
        s.sim = r->kind == LAP_RADIO_KIND_SIM;
        snprintf(s.radio_name, sizeof(s.radio_name), "%s", r->name);
        if (s.h > 0) snprintf(s.status, sizeof(s.status), "opening %s", r->name);
        else snprintf(s.status, sizeof(s.status), "%s: %s", r->name, rx_error(s.h));
        return;
    }
    s.radio_name[0] = 0;
    snprintf(s.status, sizeof(s.status), "%s", s.nradios > 0 ? "no free radio with a spectrum" : "no radio found");
}

/* The radio list fills a moment after the app opens; past this many looks
   with nothing usable, the board has no radio for a spectrum. */
#define TRIES_BEFORE_NONE 6

static void pump(void)
{
    if (s.h <= 0) {
        /* quickly at first, while the list fills; then once a second until one is plugged in */
        if (s.tries < TRIES_BEFORE_NONE ? lap_frame() % 4 == 0 : lap_frame() % 25 == 0) open_rx();
        return;
    }
    const int st = lap_rx_state(s.h);
    if (st < 0) { snprintf(s.status, sizeof(s.status), "%s: %s", s.radio_name, rx_error(st)); close_rx(); return; }
    if (st != LAP_RX_RUNNING) return;
    if (lap_rx_spectrum(s.h, s.db, BINS, &s.seq) != BINS) return;   /* no new line yet */
    lap_rx_status_t rs;
    uint32_t span = 2000000;
    if (lap_rx_status(s.h, &rs) == 0) span = rs.rate_hz;
    const lap_wf_line_t line = { s.hz, span, -100, -20, false };
    lap_wf_push(s.db, BINS, &line);
    s.peak_db = -200;
    for (int i = 0; i < BINS; i++) if (s.db[i] > s.peak_db) s.peak_db = s.db[i];
    s.lines++;
    snprintf(s.status, sizeof(s.status), "%s  peak %.0f dBFS", s.radio_name, s.peak_db);
}

/* No radio for a spectrum: say so plainly, list what the board has, and
   what would work. The app keeps looking, so plugging one in is enough. */
static void draw_no_radio(int x, int y, int w, int h)
{
    static const char *const ART[] = { "   \\ | /   ", "    \\|/    ", "  ---+---  ", "     |     ", "  .--+--.  ", "  |  x  |  " };
    const uint8_t warn = LAP_ATTR(LAP_UI_WARN, LAP_BLACK), dim = LAP_UI_DIM, text = LAP_UI_TEXT;
    lap_box(x, y, w, h, "NO RADIO", LAP_ATTR(LAP_UI_FRAME, LAP_BLACK));
    for (int i = 0; i < 6 && y + 2 + i < y + h - 1; i++)
        lap_put_str(x + 2, y + 2 + i, ART[i], i < 4 ? LAP_ATTR(LAP_CYAN, LAP_BLACK) : warn);
    int row = y + 2;
    const int tx = x + 15;
    lap_put_str(tx, row++, "No radio here gives a spectrum.", warn);
    row++;
    if (s.nradios == 0) {
        lap_put_str(tx, row++, "This board reports no radios.", text);
    } else {
        lap_put_str(tx, row++, "What this board has:", text);
        for (int i = 0; i < s.nradios && row < y + h - 6; i++) {
            char line[64];
            snprintf(line, sizeof(line), "%-8s %s", s.radios[i].name,
                     s.radios[i].why[0] ? s.radios[i].why : s.radios[i].busy ? "busy" : "no spectrum");
            lap_put_str(tx + 1, row++, line, dim);
        }
    }
    row++;
    lap_put_str(x + 2, y + 9 > row ? y + 9 : row, "Plug an RTL-SDR or HackRF into USB, or fit an", text);
    lap_put_str(x + 2, (y + 9 > row ? y + 9 : row) + 1, "LR2021 in the LoRa socket: it opens by itself.", text);
}

/* ---- choosing: RADIO list, TUNE keypad, OPTIONS ------------------------- */

static void radio_chosen(int index) { s.radio = index; open_rx(); }
static void pick_radio(void)
{
    if (s.nradios <= 0 || !lap_picker_open("RADIO", radio_chosen)) return;
    for (int i = 0; i < s.nradios; i++)
        lap_picker_add(s.radios[i].name, s.radios[i].why[0] ? s.radios[i].why : "ready");
}

static void tuned(double mhz)
{
    s.hz = (uint64_t)(mhz * 1e6 + 0.5);
    if (s.h > 0) lap_rx_tune(s.h, s.hz);
}

static double opt_get(int id) { return id == 0 ? s.hz / 1e6 : 0; }
static void opt_set(int id, double v) { if (id == 0) tuned(v); }
static const lap_opt_t OPT_ROWS[] = {
    { .label = "FREQUENCY", .kind = LAP_OPT_NUMBER, .id = 0, .lo = 1, .hi = 6000, .unit = "MHz" },
    { .label = "DISPLAY", .kind = LAP_OPT_DISPLAY },
};
static const lap_opt_list_t OPTIONS = { "@NAME@", OPT_ROWS, 2, opt_get, opt_set };

static bool act(char ch)
{
    switch (ch | 0x20) {
    case 'r': pick_radio(); return true;
    case 't': return lap_numpad_open("TUNE", "MHz", s.hz / 1e6, tuned);
    case 'o': return lap_options_open(&OPTIONS);
    default: return false;
    }
}

/* ---- the screen ---------------------------------------------------------- */

static void draw(void)
{
    const int w = lap_cols(), h = lap_rows();
    lap_clear(LAP_UI_DIM);
    pump();
    const int kh = lap_ui_keys_rows(3), ky = h - kh;
    lap_ui_bezel(0, 0, w, 10, "@NAME@");
    const lap_chip_t left[] = { { "RX", s.h > 0, LAP_GREEN }, { "SPEC", s.lines > 0, LAP_CYAN } };
    lap_ui_rail(1, 1, w - 2, left, 2, NULL, 0);
    lap_ui_frequency(1, 2, w - 2, 7, s.hz, NULL);
    lap_put_str(1, 10, s.status, s.h > 0 ? LAP_UI_TEXT : LAP_ATTR(LAP_UI_WARN, LAP_BLACK));
    if (s.h <= 0 && s.tries >= TRIES_BEFORE_NONE) {
        draw_no_radio(0, 11, w, ky - 11 < 16 ? ky - 11 : 16);
    } else {
        if (s.sim && s.h > 0)
            lap_put_str(w - 22 > 1 ? w - 22 : 1, 10, "SIM radio: test signal", LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK));
        lap_wf_draw(0, 11, w, ky - 11, LAP_WF_MINI);
    }

    s.keys[0] = (lap_btn_t){ "RADIO", s.radio_name[0] ? s.radio_name : s.tries >= TRIES_BEFORE_NONE ? "none" : "auto", 'r',
                             false, s.nradios == 0, false };
    s.keys[1] = (lap_btn_t){ "TUNE", NULL, 't', false, false, false };
    s.keys[2] = (lap_btn_t)LAP_BTN_OPTIONS;
    lap_ui_keys(0, ky, w, kh, s.keys, 3, -1);
}

static bool key(lap_key_t k, char ch) { return k == LAP_KEY_CHAR && act(ch); }

static bool touch(int col, int row)
{
    static const char LETTERS[] = "rto";
    const int k = lap_ui_key_hit(col, row);
    return k >= 0 && k < 3 && act(LETTERS[k]);
}

static void enter(void)
{
    memset(&s, 0, sizeof(s));
    s.radio = -1;
    s.hz = 433920000u;
    open_rx();
}

static void leave(void) { close_rx(); }

LAP_APP(.id = "@ID@", .name = "@NAME@", .sub = "@SUB@", .version = "0.1",
        .author = "@AUTHOR@", .color = LAP_GREEN, .flags = LAP_CAP_RADIO_RX,
        .icon_art = { "    /\\    ", "   /  \\   ", "__/    \\__", "~.~.~.~.~.", ".~.~.~.~.~", "@ART@" },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
