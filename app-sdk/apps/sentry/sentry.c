/* SENTRY: a carrier watcher that keeps watching after you leave it.
 *
 * The SDK 2.1 service example. It holds one spectrum session on a radio
 * (lap_rx_open with LAP_RX_SPECTRUM_ONLY) and, from a background tick
 * (lap_service_start, LAP_CAP_SERVICE), measures the channel it watches
 * against the noise floor every 200 ms. When a carrier comes up it posts a
 * notice (or an alert) and puts a decode on the board's timeline with
 * lap_emit, tagged "sentry", which any app or the radio head can read; when
 * it has been quiet for a second and a half it says so on the timeline too.
 *
 * The tick is cheap and bounded: one spectrum line of 128 bins, a max over
 * a few of them, a mean, a shift of the history. It never draws. RADIOS
 * lists it with STOP; WATCH here does the same.
 *
 * The screen is the board's controls around SENTRY's own tower: the glass
 * frequency, the annunciator rail, a braille history of the channel level
 * with the carrier's on/off tape under it, the VFD bargraph, the tap keys
 * and one OPTIONS list.
 *
 * The radio is tuned WATCH_OFFSET below the channel so the channel never
 * sits on the receiver's own centre spur (the usual offset-tuning trick). */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include <stdio.h>
#include <string.h>

#define A(fg) LAP_ATTR((fg), LAP_BLACK)
#define BINS 128
#define HIST 128
#define WATCH_OFFSET 20000
#define QUIET_MS 1500
#define LOG 16

LAP_ICON_PX("....................",
            "........bbbb........",
            ".......b3bb3b.......",
            "........bbbb........",
            ".........99.........",
            ".........99.........",
            "........9..9........",
            "........9..9........",
            ".......9....9.......",
            ".......9....9.......",
            "......99999999......",
            "......9......9......",
            ".....9........9.....",
            ".....9........9.....",
            "....999999999999....",
            "....................",
            "..6.6.6.6.6.6.6.6...",
            "....................");

static struct {
    bool ready;                       /* allocated and set up: survives the screen closing */
    lap_radio_info_t radios[LAP_RADIOS_MAX];
    int nradios, radio, h;
    uint64_t watch;                   /* the channel watched */
    uint32_t rate;
    int threshold;                    /* dB over the floor */
    int width;                        /* WIDTHS index */
    bool alert, notify;
    uint32_t seq, retry_ms;
    float db[BINS];
    float floor_db, level;            /* level: the channel over the floor, dB */
    float hist[HIST];                 /* level, oldest first */
    uint8_t tape[HIST];               /* carrier up at that line */
    bool up;
    int above;
    uint32_t up_ms, quiet_ms, last_len_ms;
    float up_peak;
    uint32_t count;
    char why[48];
    lap_service_t svc;
    char key_text[2][16];
    lap_btn_t keys[4];
    lap_opt_t rows[5];
    lap_opt_list_t options;
    int ky, kh;
    /* Everything the board reads lives here, never on the stack: the
       board takes app globals and malloc memory only. */
    lap_decoded_t out;
    lap_event_t in;
    char notice[64];
    lap_chip_t left[3], right[1];
    struct entry { char text[80], source[16]; uint32_t at_ms; } log[LOG];
    int log_head, logged;
} s;

static const char *const WIDTHS[] = { "5 kHz", "25 kHz", "100 kHz" };
static const uint32_t WIDTH_HZ[] = { 5000, 25000, 100000 };

static void fmt_mhz(char *out, size_t cap, uint64_t hz)
{
    snprintf(out, cap, "%lu.%03lu", (unsigned long)(hz / 1000000u), (unsigned long)(hz / 1000u % 1000u));
}

/* ---- the radio ------------------------------------------------------------- */

static void close_rx(void)
{
    if (s.h > 0) lap_rx_close(s.h);
    s.h = 0;
    s.seq = 0;
}
static bool usable(const lap_radio_info_t *r) { return r->present && !r->busy && (r->features & LAP_RXF_SPECTRUM); }
static void open_rx(void)
{
    close_rx();
    s.nradios = lap_radio_list(s.radios, LAP_RADIOS_MAX);
    if (s.radio < 0 || s.radio >= s.nradios || !usable(&s.radios[s.radio]))
        for (s.radio = 0; s.radio < s.nradios && !usable(&s.radios[s.radio]); s.radio++) {}
    if (s.radio >= s.nradios) { s.radio = -1; snprintf(s.why, sizeof(s.why), "no radio with a spectrum"); return; }
    const lap_radio_info_t *r = &s.radios[s.radio];
    s.rate = 250000;
    if (s.rate > r->max_rate) s.rate = r->max_rate;
    if (s.rate < r->min_rate) s.rate = r->min_rate;
    if (s.watch < r->min_hz + WATCH_OFFSET || s.watch > r->max_hz) s.watch = (r->min_hz + r->max_hz) / 2;
    const lap_rx_config_t cfg = { s.watch - WATCH_OFFSET, s.rate, LAP_GAIN_AUTO, LAP_RX_SPECTRUM_ONLY };
    s.h = lap_rx_open(s.radio, &cfg);
    if (s.h < 0) snprintf(s.why, sizeof(s.why), "%s would not open (%d)", r->name, s.h);
    else s.why[0] = 0;
}

/* ---- what happened -------------------------------------------------------------- */

static void tell(bool up)
{
    char mhz[16];
    fmt_mhz(mhz, sizeof(mhz), s.watch);
    lap_decoded_t *d = &s.out;
    memset(d, 0, sizeof(*d));
    d->kind = LAP_EV_TEXT;
    d->address = (uint32_t)(s.watch / 1000u);      /* the channel, in kHz */
    snprintf(d->from, sizeof(d->from), "SENTRY");
    if (up) {
        snprintf(s.notice, sizeof(s.notice), "%s MHz +%d dB", mhz, (int)s.up_peak);
        if (s.alert) lap_alert("CARRIER UP", s.notice, LAP_ALERT_WARN);
        else if (s.notify) lap_notify("CARRIER UP", s.notice);
        snprintf(d->text, sizeof(d->text), "carrier up %s MHz +%d dB", mhz, (int)s.up_peak);
    } else
        snprintf(d->text, sizeof(d->text), "carrier down %s MHz after %lu.%lu s, peak +%d dB", mhz,
                 (unsigned long)(s.last_len_ms / 1000u), (unsigned long)(s.last_len_ms / 100u % 10u), (int)s.up_peak);
    lap_emit(d);                                    /* on the board's timeline, tagged "sentry" */
}

/* One spectrum line, if a new one is waiting: the service tick, or the
   screen when the service is off. A few hundred operations, no waiting. */
static void watch(void)
{
    const uint32_t now = lap_millis();
    if (s.h <= 0 || lap_rx_state(s.h) < 0) {
        if (now - s.retry_ms > 2000) { s.retry_ms = now; open_rx(); }
        return;
    }
    uint32_t seq = 0;
    if (lap_rx_spectrum(s.h, s.db, BINS, &seq) != BINS || seq == s.seq) return;
    s.seq = seq;
    float sum = 0;
    for (int i = 0; i < BINS; i++) sum += s.db[i];
    const float mean = sum / BINS;
    s.floor_db = s.floor_db == 0 ? mean : s.floor_db * 0.95f + mean * 0.05f;
    /* The channel sits WATCH_OFFSET above the centre: its bins, +- half the width. */
    const int at = (int)(((int64_t)WATCH_OFFSET + s.rate / 2) * BINS / s.rate);
    int half = (int)((uint64_t)WIDTH_HZ[s.width] * BINS / s.rate / 2);
    if (half < 1) half = 1;
    float mx = -200;
    for (int i = at - half; i <= at + half; i++)
        if (i >= 0 && i < BINS && s.db[i] > mx) mx = s.db[i];
    s.level = mx - s.floor_db;
    memmove(s.hist, s.hist + 1, sizeof(s.hist) - sizeof(s.hist[0]));
    memmove(s.tape, s.tape + 1, sizeof(s.tape) - 1);
    s.hist[HIST - 1] = s.level < 0 ? 0 : s.level;
    if (s.level >= (float)s.threshold) {
        s.quiet_ms = now;
        if (s.up && s.level > s.up_peak) s.up_peak = s.level;
        if (!s.up && ++s.above >= 2) {          /* two lines in a row: not a blip */
            s.up = true;
            s.up_ms = now;
            s.up_peak = s.level;
            s.count++;
            tell(true);
        }
    } else {
        s.above = 0;
        if (s.up && now - s.quiet_ms > QUIET_MS) {
            s.up = false;
            s.last_len_ms = s.quiet_ms - s.up_ms;
            tell(false);
        }
    }
    s.tape[HIST - 1] = s.up;
}

static void tick(void) { watch(); }
static void stopped(void) { close_rx(); }       /* the board would close it too */
static void start_service(void)
{
    s.svc = (lap_service_t){ tick, stopped, 200 };
    if (lap_service_start(&s.svc) != LAP_SVC_OK) snprintf(s.why, sizeof(s.why), "background not allowed");
}

/* ---- OPTIONS and the keypad --------------------------------------------------------- */

enum { OPT_THRESHOLD, OPT_WIDTH, OPT_NOTIFY, OPT_ALERT, OPT_FREQ };
static double opt_get(int id)
{
    switch (id) {
    case OPT_THRESHOLD: return s.threshold;
    case OPT_WIDTH: return s.width;
    case OPT_NOTIFY: return s.notify;
    case OPT_ALERT: return s.alert;
    default: return (double)(s.watch / 1000u) / 1000.0;
    }
}
static void retune(uint64_t hz)
{
    s.watch = hz;
    s.up = false;
    s.above = 0;
    if (s.h > 0 && lap_rx_tune(s.h, hz - WATCH_OFFSET) != 0) open_rx();
}
static void opt_set(int id, double v)
{
    switch (id) {
    case OPT_THRESHOLD: s.threshold = (int)v; break;
    case OPT_WIDTH: s.width = (int)v % 3; break;
    case OPT_NOTIFY: s.notify = v > 0.5; break;
    case OPT_ALERT: s.alert = v > 0.5; break;
    default: retune((uint64_t)(v * 1e6 + 0.5)); break;
    }
}
static void open_options(void)
{
    s.rows[0] = (lap_opt_t){ .label = "THRESHOLD", .kind = LAP_OPT_LEVEL, .id = OPT_THRESHOLD, .lo = 6, .hi = 40, .step = 3, .unit = "dB" };
    s.rows[1] = (lap_opt_t){ .label = "CHANNEL WIDTH", .kind = LAP_OPT_CYCLE, .id = OPT_WIDTH, .names = WIDTHS, .n = 3 };
    s.rows[2] = (lap_opt_t){ .label = "NOTICE", .kind = LAP_OPT_TOGGLE, .id = OPT_NOTIFY };
    s.rows[3] = (lap_opt_t){ .label = "ALERT INSTEAD", .kind = LAP_OPT_TOGGLE, .id = OPT_ALERT };
    s.rows[4] = (lap_opt_t){ .label = "CHANNEL", .kind = LAP_OPT_NUMBER, .id = OPT_FREQ, .lo = 1, .hi = 6000, .unit = "MHz" };
    s.options = (lap_opt_list_t){ "SENTRY", s.rows, 5, opt_get, opt_set };
    lap_options_open(&s.options);
}
static void typed(double mhz) { retune((uint64_t)(mhz * 1e6 + 0.5)); }

/* ---- the screen ------------------------------------------------------------------------ */

/* SENTRY's tower: the lamp is the carrier, lit only while one is up. */
static void draw_tower(int x, int y)
{
    const uint8_t stone = A(LAP_BLUE | LAP_BRIGHT), lamp = s.up ? A(LAP_YELLOW | LAP_BRIGHT) : A(LAP_BLACK | LAP_BRIGHT);
    lap_put_str(x, y + 0, s.up ? " \\ | / " : "       ", A(LAP_YELLOW));
    lap_put_str(x, y + 1, "  .-.  ", stone);
    lap_put_str(x, y + 2, " ( ", stone);
    lap_put_cell(x + 3, y + 2, s.up ? '@' : 'o', lamp);
    lap_put_str(x + 4, y + 2, " ) ", stone);
    lap_put_str(x, y + 3, "  |=|  ", stone);
    lap_put_str(x, y + 4, "  | |  ", A(LAP_CYAN));
    lap_put_str(x, y + 5, " /| |\\ ", A(LAP_CYAN));
    lap_put_str(x, y + 6, "/_|_|_\\", A(LAP_CYAN));
}

static void draw(void)
{
    if (!lap_service_running()) watch();            /* without the service, only while open */
    const int W = lap_cols(), H = lap_rows();
    lap_clear(LAP_UI_DIM);
    s.kh = lap_ui_keys_rows(4);
    s.ky = H - s.kh;
    const uint32_t now = lap_millis();

    draw_tower(0, 0);
    lap_put_str(8, 0, "SENTRY", A(LAP_GREEN | LAP_BRIGHT));
    char t[64], mhz[16];
    fmt_mhz(mhz, sizeof(mhz), s.watch);
    if (s.up)
        snprintf(t, sizeof(t), "CARRIER UP  +%d dB  %lu s", (int)s.level, (unsigned long)((now - s.up_ms) / 1000u));
    else if (s.h > 0 && s.seq)
        snprintf(t, sizeof(t), "quiet  %+d dB", (int)s.level);
    else
        snprintf(t, sizeof(t), "%s", s.why[0] ? s.why : "opening the radio");
    lap_put_str(16, 0, t, s.up ? A(LAP_YELLOW | LAP_BRIGHT) : s.h > 0 ? LAP_UI_DIM : A(LAP_RED | LAP_BRIGHT));
    const lap_radio_info_t *r = s.radio >= 0 && s.radio < s.nradios ? &s.radios[s.radio] : NULL;
    const bool svc = lap_service_running();
    s.left[0] = (lap_chip_t){ svc ? "WATCHING" : "ON SCREEN", true, svc ? LAP_UI_LIVE : LAP_UI_WARN };
    s.left[1] = (lap_chip_t){ "UP", s.up, LAP_UI_VALUE };
    s.left[2] = (lap_chip_t){ s.alert ? "ALERT" : "NOTICE", s.alert || s.notify, LAP_CYAN };
    s.right[0] = (lap_chip_t){ r ? r->name : "NO RADIO", r != NULL, LAP_MAGENTA };
    lap_ui_rail(8, 1, W - 8, s.left, 3, s.right, 1);

    /* The channel, in the board's glass figures. */
    /* Beside the tower when there is width for the figures, under it when not. */
    const bool beside = W >= 70;
    int y = beside ? lap_ui_frequency(8, 2, W - 8, 8, s.watch, NULL) : lap_ui_frequency(0, 7, W, 8, s.watch, NULL);
    if (y < (beside ? 8 : 9)) y = beside ? 8 : 9;

    /* The board's timeline as any reader sees it: what was decoded, and by
       whom (SENTRY's own carry "sentry"). Read every frame, kept in a ring. */
    while (lap_event_next(&s.in) == 1) {
        s.log_head = (s.log_head + 1) % LOG;
        snprintf(s.log[s.log_head].text, sizeof(s.log[0].text), "%.79s", s.in.text);
        lap_event_source(s.log[s.log_head].source, sizeof(s.log[0].source));
        s.log[s.log_head].at_ms = now;
        if (s.logged < LOG) s.logged++;
    }

    /* The level: a braille history with the threshold on its rule and the
       carrier's on/off tape under it; the VFD bar is the line just in. */
    const int room = s.ky - y - 1;
    const int shown = room > 12 ? (room - 6 < LOG ? room - 6 : LOG) : 0;   /* timeline rows, portrait */
    int hh = room - 4 - (shown ? shown + 2 : 0);
    if (hh > 18) hh = 18;
    if (hh < 2) hh = 2;
    const int hy = y + 1;
    lap_box(0, hy - 1, W, hh + 3, " LEVEL ", A(LAP_BLUE));
    snprintf(t, sizeof(t), " trip %d dB ", s.threshold);
    lap_put_str(W - (int)strlen(t) - 1, hy - 1, t, A(LAP_YELLOW));
    lap_ui_history(1, hy, W - 2, hh, s.hist, HIST, 0, 60, s.up ? LAP_YELLOW | LAP_BRIGHT : LAP_CYAN | LAP_BRIGHT);
    for (int c = 0; c < W - 2; c++) {
        const int i = HIST - (W - 2 - c) * 2;      /* two lines a cell, newest right, as the history */
        const bool on = (i >= 0 && s.tape[i]) || (i + 1 >= 0 && i + 1 < HIST && s.tape[i + 1]);
        lap_put_cell(1 + c, hy + hh, on ? '=' : '-', on ? A(LAP_YELLOW | LAP_BRIGHT) : LAP_UI_FAINT);
    }
    const int by = hy + hh + 2;
    lap_ui_bargraph(1, by, W - 12, 1, s.level / 60.0f, (float)s.threshold / 60.0f);
    snprintf(t, sizeof(t), "%3lu up", (unsigned long)s.count);
    lap_put_str(W - 9, by, t, LAP_UI_TEXT);

    if (shown) {
        lap_box(0, by + 1, W, shown + 2, " TIMELINE ", A(LAP_MAGENTA));
        for (int i = 0; i < shown && i < s.logged; i++) {
            const struct entry *e = &s.log[(s.log_head - i + LOG) % LOG];
            const int ry = by + 2 + i;
            const bool up = !strncmp(e->text, "carrier up", 10);
            lap_put_cell(1, ry, up ? '^' : 'v', up ? A(LAP_YELLOW | LAP_BRIGHT) : A(LAP_CYAN));
            snprintf(t, sizeof(t), "%s", e->source[0] ? e->source : "board");
            lap_put_str(3, ry, t, A(LAP_MAGENTA | LAP_BRIGHT));
            lap_put_str(4 + (int)strlen(t), ry, e->text, i ? LAP_UI_DIM : LAP_UI_TEXT);
            snprintf(t, sizeof(t), " %lus ", (unsigned long)((now - e->at_ms) / 1000u));
            lap_put_str(W - (int)strlen(t) - 1, ry, t, LAP_UI_FAINT);
        }
        if (!s.logged) lap_put_str(2, by + 2, "nothing on the board's timeline yet", LAP_UI_FAINT);
    } else if (s.logged) {
        const struct entry *e = &s.log[s.log_head];
        snprintf(t, sizeof(t), " [%s] ", e->source[0] ? e->source : "board");
        lap_put_str(0, by + 1, t, A(LAP_MAGENTA | LAP_BRIGHT));
        lap_put_str((int)strlen(t), by + 1, e->text, LAP_UI_DIM);
    }

    snprintf(s.key_text[0], sizeof(s.key_text[0]), "%s", svc ? "on" : "off");
    snprintf(s.key_text[1], sizeof(s.key_text[1]), "%s", mhz);
    s.keys[0] = (lap_btn_t){ "WATCH", s.key_text[0], 'w', svc, false, false };
    s.keys[1] = (lap_btn_t){ "TUNE", s.key_text[1], 't', false, false, false };
    s.keys[2] = (lap_btn_t){ "RADIO", r ? r->name : "none", 'r', false, false, false };
    s.keys[3] = (lap_btn_t){ "OPTIONS", NULL, 'o', false, false, false };
    lap_ui_keys(0, s.ky, W, s.kh, s.keys, 4, -1);
}

static void next_radio(void)
{
    s.radio = s.nradios ? (s.radio + 1) % s.nradios : -1;
    open_rx();
}
static bool key(lap_key_t k, char ch)
{
    if (k != LAP_KEY_CHAR) return false;
    switch (ch) {
    case 'w': if (lap_service_running()) lap_service_stop(); else start_service(); return true;
    case 't': lap_numpad_open("CHANNEL", "MHz", (double)(s.watch / 1000u) / 1000.0, typed); return true;
    case 'r': next_radio(); return true;
    case 'o': open_options(); return true;
    default: return false;
    }
}
static bool touch(int col, int row)
{
    const int k = lap_ui_key_hit(col, row);
    return k >= 0 ? key(LAP_KEY_CHAR, s.keys[k].key) : row >= s.ky;
}

static void enter(void)
{
    if (s.ready) return;                      /* back from the background: all of it is still here */
    memset(&s, 0, sizeof(s));
    s.ready = true;
    s.radio = -1;
    s.watch = 433920000u;
    s.threshold = 15;
    s.width = 1;
    s.notify = true;
    lap_event_subscribe(LAP_EV_TEXT);         /* so the board keeps its timeline for readers */
    open_rx();
    start_service();
}
static void leave(void)
{
    if (lap_service_running()) return;        /* it goes on watching: keep the session */
    close_rx();
    s.ready = false;
}

LAP_APP(.id = "sentry", .name = "SENTRY", .sub = "carrier watch, also closed", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_TOWER, .color = LAP_GREEN,
        .flags = LAP_CAP_RADIO_RX | LAP_CAP_SERVICE,
        .icon_art = { "   .-.    ", "  ( @ )   ", "   |=|    ", "   | |    ", "  /| |\\   ", " SENTRY   " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
