/* @NAME@: @SUB@.
 *
 * Made by `lsdk new` from the blank template. A header emblem in character
 * art, one live value, the board's tap keys and its OPTIONS list.
 *
 * Keys: h says hello, o opens OPTIONS.
 *   lsdk build      compile for the board
 *   lsdk sim        run it on the PC
 *   lsdk install    put it on the board */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include <stdio.h>
#include <string.h>

/* The tile's colour icon: 20 x 18 dots, '0'-'f' are theme colours. */
LAP_ICON_PX("....................",
            "....................",
            ".......ee...........",
            "......eeee..........",
            ".....eeeeee.........",
            "....eeeeeeee........",
            "...eeeeeeeeee.......",
            "..eeeeeeeeeeee......",
            "....................",
            "4444444444444444444.",
            ".c..c..c..c..c..c...",
            "....................",
            "..6666666666666666..",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................");

static const char *const EMBLEM[] = {
    "    /|    ",
    "   / |    ",
    "  /__|__  ",
    "~~~~~~~~~~",
};

static struct {
    int hellos;
    bool chirp;                 /* OPTIONS: a tone with each hello */
    uint32_t last_ms;
    char count[12];             /* the HELLO key's value: app memory, not the stack */
    lap_btn_t keys[2];
} s;

/* ---- OPTIONS ----------------------------------------------------------- */

enum { OPT_CHIRP, OPT_RESET };
static double opt_get(int id) { return id == OPT_CHIRP ? s.chirp : 0; }
static void opt_set(int id, double v)
{
    if (id == OPT_CHIRP) s.chirp = v > 0.5;
    if (id == OPT_RESET) s.hellos = 0;
}
static const lap_opt_t OPT_ROWS[] = {
    { .label = "CHIRP", .kind = LAP_OPT_TOGGLE, .id = OPT_CHIRP },
    { .label = "RESET COUNT", .kind = LAP_OPT_ACTION, .id = OPT_RESET },
};
static const lap_opt_list_t OPTIONS = { "@NAME@", OPT_ROWS, 2, opt_get, opt_set };

/* ---- the screen -------------------------------------------------------- */

static void hello(void)
{
    s.hellos++;
    s.last_ms = lap_millis();
    if (s.chirp) lap_tone(880, 60, 40, LAP_WAVE_TRIANGLE);
    lap_log("hello number %d", s.hellos);
}

static void draw(void)
{
    const int w = lap_cols(), h = lap_rows();
    lap_clear(LAP_UI_DIM);

    /* the face: emblem, name, one line saying what it is */
    for (int i = 0; i < 4; i++)
        lap_put_str(1, 1 + i, EMBLEM[i], LAP_ATTR(i == 3 ? LAP_BLUE | LAP_BRIGHT : LAP_CYAN | LAP_BRIGHT, LAP_BLACK));
    lap_put_str(13, 1, "@NAME@", LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK));
    lap_put_str(13, 2, "@SUB@", LAP_UI_DIM);
    lap_hline(0, 6, w, '-', LAP_UI_FAINT);

    /* what is happening now: the count, big, and how long since the last */
    const int kh = lap_ui_keys_rows(2), ky = h - kh;
    lap_box(1, 7, w - 2, ky - 8 < 12 ? ky - 8 : 12, "HELLOS", LAP_ATTR(LAP_UI_FRAME, LAP_BLACK));
    snprintf(s.count, sizeof(s.count), "%d", s.hellos % 1000);
    for (int i = 0; s.count[i]; i++) lap_big_char(3 + i * 4, 9, s.count[i], LAP_ATTR(LAP_UI_LIVE, LAP_BLACK));
    if (s.hellos) {
        char ago[32];
        snprintf(ago, sizeof(ago), "last one %lus ago", (unsigned long)((lap_millis() - s.last_ms) / 1000));
        lap_put_str(3, 15, ago, LAP_UI_TEXT);
        lap_ui_meter(3, 16, w - 8, (s.hellos % 10) / 10.0f, LAP_UI_VALUE);
    } else {
        lap_put_str(3, 15, "press HELLO (h) or tap it", LAP_UI_TEXT);
    }

    /* the board's tap keys, along the bottom */
    s.keys[0] = (lap_btn_t){ "HELLO", s.count, 'h', false, false, false };
    s.keys[1] = (lap_btn_t)LAP_BTN_OPTIONS;
    lap_ui_keys(0, ky, w, kh, s.keys, 2, -1);
}

static bool key(lap_key_t k, char ch)
{
    if (k != LAP_KEY_CHAR) return false;
    switch (ch | 0x20) {
    case 'h': hello(); return true;
    case 'o': return lap_options_open(&OPTIONS);
    default: return false;
    }
}

static bool touch(int col, int row)
{
    const int k = lap_ui_key_hit(col, row);
    if (k == 0) { hello(); return true; }
    if (k == 1) return lap_options_open(&OPTIONS);
    return false;
}

static void enter(void)
{
    memset(&s, 0, sizeof(s));
}

LAP_APP(.id = "@ID@", .name = "@NAME@", .sub = "@SUB@", .version = "0.1",
        .author = "@AUTHOR@", .color = LAP_CYAN,
        .icon_art = { "    /|    ", "   / |    ", "  /__|__  ", "~~~~~~~~~~", "          ", "@ART@" },
        .enter = enter, .draw = draw, .key = key, .touch = touch);
