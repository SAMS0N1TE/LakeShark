/* LakeShark's own controls for native apps (API 2.1).
 *
 * The keys, OPTIONS lists, pickers, keypads, radio glass, meters and
 * waterfall the board's screens are built from, so an app looks and behaves
 * like FM, P25 or RADIOS with a few calls instead of a few hundred lines.
 * They ARE the board's: the same code draws them, so they change when the
 * board's look changes and never drift from it. Do not restyle them.
 *
 *     #include "lakeshark_app.h"
 *     #include "lakeshark_ui.h"
 *
 * Positions are the app's own cells, as for lap_put_str. Every call copies
 * what it is given (labels, tables, values) and clips to the app's area, so
 * nothing an app passes can draw outside it or outlive it. Drawing calls
 * work only inside draw(); the lists and keypads open from any callback and
 * call the app back on the screen task when the owner chooses. Anything
 * still open when the app closes is closed with it.
 *
 * Needs firmware 2.21 (API 2.1). Not available with LAP_TARGET_API_1/_20. */
#ifndef LAKESHARK_UI_H
#define LAKESHARK_UI_H

#include "lakeshark_app.h"

#if defined(LAP_API_21)
#ifdef __cplusplus
extern "C" {
#endif

/* ---- colours: the board's tokens ------------------------------------- */

/* The board's screens use a small set of meanings. Keep to them and an app
   reads like the rest: words in TEXT or DIM (FAINT carries no words, only
   rules, unlit scales and grid), frames in FRAME, live values in LIVE, a
   setting's value in VALUE, trouble in WARN and ALARM. */
#define LAP_UI_TEXT   LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK)
#define LAP_UI_DIM    LAP_ATTR(LAP_WHITE, LAP_BLACK)
#define LAP_UI_FAINT  LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK)
#define LAP_UI_FRAME  LAP_CYAN
#define LAP_UI_LIVE   (LAP_GREEN | LAP_BRIGHT)
#define LAP_UI_VALUE  (LAP_YELLOW | LAP_BRIGHT)
#define LAP_UI_WARN   (LAP_YELLOW | LAP_BRIGHT)
#define LAP_UI_ALARM  (LAP_RED | LAP_BRIGHT)

/* ---- tap keys ---------------------------------------------------------- */

/* The filled keys along the bottom of every board screen: label, the value
   under it, the keyboard letter on its corner. At least 3 rows x 8 columns
   each, as the board's own. */
typedef struct {
    const char *label;          /* short, upper case: "RADIO" */
    const char *value;          /* the setting under it, or NULL */
    char key;                   /* its letter, 0 for none */
    bool on;                    /* drawn engaged */
    bool dim;                   /* unavailable right now: shown, not pressable */
    bool badge;                 /* something new inside */
} lap_btn_t;
#define LAP_KEYS_MAX 12
/* The rows a bar of n keys wants at the bottom of this app's area, the way
   the board sizes its own (taller in portrait, one row per 10 columns). */
int  lap_ui_keys_rows(int n);
/* Draw n keys (up to LAP_KEYS_MAX) in x, y, w, h; focus is the key the
   keyboard is on, -1 for none. */
void lap_ui_keys(int x, int y, int w, int h, const lap_btn_t *keys, int n, int focus);
/* Which key of the last lap_ui_keys a tap at col, row landed on, or -1 (a
   dim key is a miss). Which key has letter ch, or -1. */
int  lap_ui_key_hit(int col, int row);
int  lap_ui_key_find(char ch);

/* ---- OPTIONS: the layered settings list -------------------------------- */

/* One OPTIONS button on 'o' opens one list titled "<NAME> OPTIONS", a row
   per setting with its value beside it. Rows that are choices step in place;
   numbers open the keypad; MENU rows open a list of their own with a BACK
   to this one. The same list every board screen uses (FM, P25, RADIOS).
   Tables are copied when opened (up to 6 lists and 48 rows in all, 8 names
   a row); the values are asked for through get while the list is up. */
enum {
    LAP_OPT_CYCLE = 0,          /* steps through names[0..n-1], wrapping */
    LAP_OPT_TOGGLE,             /* off/on; names = the two words, or NULL */
    LAP_OPT_NUMBER,             /* typed on the keypad, lo..hi, unit under the title */
    LAP_OPT_ACTION,             /* does something: set(id, 0) */
    LAP_OPT_MENU,               /* opens sub */
    LAP_OPT_LEVEL,              /* < and > move it by step, lo..hi; tapping it types one */
    LAP_OPT_DISPLAY,            /* opens the shared waterfall's own display settings */
};
typedef struct lap_opt_list lap_opt_list_t;
typedef struct {
    const char *label;
    uint8_t kind;               /* LAP_OPT_* */
    int id;                     /* the app's number for the row, handed to get and set */
    const char *const *names;   /* CYCLE, TOGGLE */
    int n;                      /* CYCLE: how many names */
    double lo, hi, step;        /* NUMBER, LEVEL */
    const char *unit;           /* NUMBER, LEVEL */
    const lap_opt_list_t *sub;  /* MENU */
} lap_opt_t;
struct lap_opt_list {
    const char *name;           /* "SPECTRE": titled SPECTRE OPTIONS */
    const lap_opt_t *rows;
    int n;
    /* The row's value: the choice's index for CYCLE and TOGGLE, the number
       for NUMBER and LEVEL. A sub list without its own uses its parent's. */
    double (*get)(int id);
    void (*set)(int id, double value);
};
bool lap_options_open(const lap_opt_list_t *root);
bool lap_options_showing(void);
void lap_options_close(void);
/* The OPTIONS key for a lap_btn_t bar: { "OPTIONS", NULL, 'o' }. */
#define LAP_BTN_OPTIONS { "OPTIONS", NULL, 'o', false, false, false }

/* ---- lists and keypads ------------------------------------------------- */

/* The board's list to pick one thing from: open it, add up to 48 rows, and
   done(index) is called with the index the row was ADDED at, or not at all
   when the owner backs out. */
bool lap_picker_open(const char *title, void (*done)(int index));
bool lap_picker_add(const char *label, const char *detail);
void lap_picker_select(int index);
/* The board's number keypad over the app; done(value) when entered. */
bool lap_numpad_open(const char *title, const char *unit, double initial, void (*done)(double value));
bool lap_ui_overlay(void);      /* a list or keypad of this app's is up */

/* FM's inline frequency keypad, drawn in place of the app's controls.
   `entry` is the digits typed so far (the app keeps them), invalid shows it
   refused. lap_ui_freq_hit says what a tap pressed: '0'-'9', '.', 'd'
   delete, 'c' clear, 'x' cancel, 't' tune, 0 a miss. */
void lap_ui_freq_pad(int x, int y, int w, int h, const char *entry, bool invalid);
char lap_ui_freq_hit(int col, int row);

/* ---- the radio glass --------------------------------------------------- */

/* The receiver face FM and P25 share. bezel: a ruled frame with its title
   on the rule. chip: an annunciator, a lit word; returns where the next one
   starts. rail: a row of chips, left ones folding from x, right ones held
   at the right edge. */
void lap_ui_bezel(int x, int y, int w, int h, const char *title);
int  lap_ui_chip(int x, int y, const char *text, bool on, uint8_t hue);
typedef struct { const char *text; bool on; uint8_t hue; } lap_chip_t;
void lap_ui_rail(int x, int y, int w, const lap_chip_t *left, int left_n,
                 const lap_chip_t *right, int right_n);
/* The big frequency figures, inside the box x, y, w, h starting at row y;
   they roll to a new frequency as FM's do. entry, when not NULL, shows
   digits being typed instead. Returns the first row below the figures. */
int  lap_ui_frequency(int x, int y, int w, int h, uint64_t hz, const char *entry);
/* A VFD bargraph: lit filaments, green through amber to red, `rows` tall;
   peak >= 0 holds a white mark. */
void lap_ui_bargraph(int x, int y, int w, int rows, float frac, float peak);
/* The VFO control: label and value on the title row, the travel under it,
   - and + keys at its end. Up to 4 on a screen (slot 0..3).
   lap_ui_vfo_hit: LAP_UI_PART_DOWN / _UP step, _VALUE opens exact entry,
   _BAR sets the value at *frac (0..1) where it was tapped; -1 a miss. */
enum { LAP_UI_PART_DOWN = 0, LAP_UI_PART_UP, LAP_UI_PART_VALUE, LAP_UI_PART_BAR };
void lap_ui_vfo(int slot, int x, int y, int w, int h, const char *label, const char *value,
                float frac, uint8_t hue);
int  lap_ui_vfo_hit(int slot, int col, int row, float *frac);

/* ---- meters and history ------------------------------------------------ */

/* One-row ASCII meter, [====|===>--+---], quarter ticks, 0..1. */
void lap_ui_meter(int x, int y, int w, float frac, uint8_t hue);
/* A sparkline in line characters (':' half, '|' full), n levels 0..max,
   one a column; marker is a column drawn in the peak colour, -1 none. */
void lap_ui_sparkline(int x, int y, int w, int h, const int *level, int n, int max, int marker);
/* History in braille: n values (up to 512) between lo and hi, two a cell,
   newest at the right, as SWEEP's strips are drawn. */
void lap_ui_history(int x, int y, int w, int h, const float *v, int n, float lo, float hi, uint8_t hue);

/* ---- the waterfall ----------------------------------------------------- */

/* The board's waterfall instrument: the same history, palettes, contrast,
   speed and pixel rendering FM, P25 and REC show, with its own display
   settings (LAP_OPT_DISPLAY in OPTIONS opens them). Push a spectrum line
   (dB, low frequency first, up to 256 bins) whenever there is a new one;
   draw it where it goes. Pushing claims the instrument for the app while
   its screen is open; closing the screen gives it back. */
typedef struct {
    uint64_t center_hz;
    uint32_t span_hz;
    float floor_db, top_db;     /* what the colours run between */
    bool gap;                   /* samples were lost before this line */
} lap_wf_line_t;
bool lap_wf_push(const float *db, int bins, const lap_wf_line_t *line);
/* style: LAP_WF_MINI the trace and history only; LAP_WF_FULL the whole
   instrument with its scale, marker readout and control keys. */
enum { LAP_WF_MINI = 0, LAP_WF_FULL = 1 };
void lap_wf_draw(int x, int y, int w, int h, int style);
/* The full instrument's keys and taps; true when it used them. */
bool lap_wf_key(lap_key_t key, char ch);
bool lap_wf_touch(int col, int row);
/* The marker's frequency, 0 when there is none. */
uint64_t lap_wf_marker_hz(void);

#ifdef __cplusplus
}
#endif
#endif /* LAP_API_21 */
#endif /* LAKESHARK_UI_H */
