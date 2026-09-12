/* LS_TEST_SOURCES: ls_keyboard.c and ls_glyph.c against their drawn output */

#include "ls_test.h"

#include "ls_glyph.h"
#include "ls_keyboard.h"
#include "tui_core.h"

#include <stdio.h>
#include <string.h>

/* The real portrait geometry, because the overlay sizes itself from the rect
   it is handed and the sizes are the thing under test. */
#define W 48
#define H 61

static tui_cell g_back[W * H];
static tui_cell g_front[W * H];
static tui_surface g_sf;

static tui_rect area(void) { return tui_rect_make(0, 0, W, H); }

static void surface_reset(void)
{
    tui_surface_setup(&g_sf, g_back, g_front, W, H);
    tui_frame_begin(&g_sf);
}

static void redraw(void)
{
    surface_reset();
    ls_keyboard_draw(&g_sf, area());
}

/* --------------------------------------------------------------- fixture -- */

static char s_got[128];
static int  s_done_calls;

static void on_done(const char *text)
{
    snprintf(s_got, sizeof(s_got), "%s", text ? text : "");
    s_done_calls++;
}

static void open_with(const char *initial)
{
    s_got[0] = 0;
    s_done_calls = 0;
    ls_keyboard_open("MESSAGE", initial, 63, on_done);
    redraw();
}

/* One row of the grid as a string. */
static void row_text(int y, char *out, size_t n)
{
    size_t k = 0;
    for (int x = 0; x < W && k + 1 < n; x++) out[k++] = g_back[y * W + x].ch;
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static bool wordish(int x, int y)
{
    if (x < 0 || x >= W) return false;
    const char c = g_back[y * W + x].ch;
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static bool find_exact(char c, int *cx, int *cy)
{
    for (int y = H - 1; y >= 0; y--)
        for (int x = 0; x < W; x++) {
            if (g_back[y * W + x].ch != c) continue;
            if (g_back[y * W + x].attr != TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)) continue;

            if (wordish(x - 1, y) || wordish(x + 1, y)) continue;
            *cx = x;
            *cy = y;
            return true;
        }
    return false;
}

/* Letters are addressed by POSITION, not by case: 'h' means the key where h
   lives, and whether it produces 'h' or 'H' is the shift state's business.
   That is also how a finger addresses it. */
static bool find_key(char c, int *cx, int *cy)
{
    if (find_exact(c, cx, cy)) return true;
    if (c >= 'a' && c <= 'z') return find_exact((char)(c - 32), cx, cy);
    if (c >= 'A' && c <= 'Z') return find_exact((char)(c + 32), cx, cy);
    return false;
}

/* Tap the key carrying `c`. Fails the case if it is not on screen. */
static void tap_key(char c)
{
    int x = -1, y = -1;
    LS_CHECK_MSG(find_key(c, &x, &y), "no key drawn for '%c'", c);
    if (x < 0) return;
    ls_keyboard_touch(x, y);
    redraw();
}

/* Find a word label - DEL, OK, CANCEL, SPACE - and tap its middle. */
static bool tap_label(const char *word)
{
    for (int y = 0; y < H; y++) {
        char line[W + 1];
        row_text(y, line, sizeof(line));
        const char *at = strstr(line, word);
        if (!at) continue;
        const int x = (int)(at - line) + (int)strlen(word) / 2;
        ls_keyboard_touch(x, y);
        redraw();
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------- cases -- */

LS_CASE(a_tapped_letter_lands_in_the_buffer)
{

    open_with("");
    tap_key('h');
    tap_key('i');

    bool found = false;
    for (int y = 0; y < H && !found; y++) {
        char line[W + 1];
        row_text(y, line, sizeof(line));
        if (strstr(line, "Hi")) found = true;
    }
    LS_CHECK_MSG(found, "typed H then i and the box does not read 'Hi'");
}

LS_CASE(an_empty_box_opens_shifted_and_drops_back_after_one_letter)
{
    /* A message starts with a capital and the second letter is not one. A
       shift that stayed on would have to be turned off by hand after every
       word, which is the thing that makes an on-screen keyboard tiring. */
    open_with("");
    int x, y;
    LS_CHECK_MSG(find_key('Q', &x, &y),
                 "an empty box did not open on the capitals");

    /* Q and q share a figure - the table is upper case forms at both cases,
       because a three by five 'g' has no room for a descender and without
       one it is a 'q' - so the shift state is not observable from the shape
       of a key. It is observable from what the key PRODUCES, which is the
       thing that matters: 'A' then 'b' is "Ab" and never "AB". */
    tap_key('a');
    tap_key('b');
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_EQ_INT(s_done_calls, 1);
    LS_CHECK_MSG(!strcmp(s_got, "Ab"), "got '%s', wanted 'Ab'", s_got);
}

LS_CASE(an_edit_of_existing_text_does_not_shift)
{

    open_with("hello");
    tap_key('a');
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_CHECK_MSG(!strcmp(s_got, "helloa"), "got '%s', wanted 'helloa'", s_got);
}

LS_CASE(the_symbol_key_reaches_the_digits_and_comes_back)
{
    /* Three layers, and the only route to two of them is this key. A layer
       you cannot leave is a keyboard that cannot finish a sentence. */
    open_with("");
    int x, y;
    LS_CHECK_MSG(!find_key('7', &x, &y), "digits were on the letter layer");

    LS_CHECK_MSG(tap_label("123"), "no layer key drawn");
    LS_CHECK_MSG(find_key('7', &x, &y), "the layer key did not reach the digits");

    LS_CHECK_MSG(tap_label("ABC"), "the layer key did not offer a way back");
    LS_CHECK_MSG(!find_key('7', &x, &y), "still on the digits after ABC");
}

LS_CASE(delete_removes_the_last_character_and_nothing_else)
{
    open_with("abc");
    LS_CHECK_MSG(tap_label("DEL"), "no DEL key drawn");
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_CHECK_MSG(!strcmp(s_got, "ab"), "got '%s', wanted 'ab'", s_got);
}

LS_CASE(cancel_closes_without_calling_back)
{

    open_with("armed");
    LS_CHECK_MSG(tap_label("CANCEL"), "no CANCEL key drawn");
    LS_EQ_INT(s_done_calls, 0);
    LS_CHECK_MSG(!ls_keyboard_active(), "CANCEL left the overlay up");
}

LS_CASE(ok_closes_and_reports_what_is_in_the_box)
{
    open_with("ready");
    LS_CHECK_MSG(tap_label("OK"), "no OK key drawn");
    LS_EQ_INT(s_done_calls, 1);
    LS_CHECK_MSG(!strcmp(s_got, "ready"), "got '%s', wanted 'ready'", s_got);
    LS_CHECK_MSG(!ls_keyboard_active(), "OK left the overlay up");
}

LS_CASE(a_tap_that_misses_every_key_does_not_reach_the_screen_behind)
{

    open_with("");
    LS_CHECK_MSG(ls_keyboard_touch(0, 0),
                 "a tap on the overlay's own border was passed through");
    LS_CHECK_MSG(ls_keyboard_active(), "a miss closed the overlay");
}

LS_CASE(a_physical_keyboard_types_into_the_same_buffer)
{
    /* The QWERTY is detachable. Plugging it in has to make this faster, not
       make the screen disagree with the keys. */
    open_with("");
    ls_keyboard_key(LS_TK_CHAR, 'x');
    ls_keyboard_key(LS_TK_CHAR, 'y');
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_CHECK_MSG(!strcmp(s_got, "xy"), "got '%s', wanted 'xy'", s_got);
}

LS_CASE(the_buffer_stops_at_the_length_the_caller_asked_for)
{
    /* A mesh message is 64 bytes including the NUL and the compose buffer is
       not the place to find that out. */
    open_with("");
    for (int i = 0; i < 200; i++) ls_keyboard_key(LS_TK_CHAR, 'a');
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_EQ_INT((int)strlen(s_got), 63);
}

LS_CASE(secret_entry_masks_text_and_preserves_exact_input)
{
    s_done_calls = 0;
    ls_keyboard_open_secret("PASSWORD", 63, on_done);
    redraw();
    tap_key('a');
    const char *tail = "bC7![]";
    for (const char *p = tail; *p; p++) ls_keyboard_key(LS_TK_CHAR, *p);
    redraw();
    bool masked = false;
    for (int y = 0; y < H; y++) {
        char row[W + 1]; row_text(y, row, sizeof(row));
        LS_CHECK(strstr(row, "abC7![]") == NULL);
        masked |= strstr(row, "*******") != NULL;
    }
    LS_CHECK(masked);
    LS_CHECK(tap_label("OK"));
    LS_EQ_INT(s_done_calls, 1);
    LS_CHECK(!strcmp(s_got, "abC7![]"));
    open_with("");
    tap_key('a');
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_CHECK(!strcmp(s_got, "A"));
}

LS_CASE(extra_password_symbols_are_reachable_by_touch)
{
    ls_keyboard_open_secret("PASSWORD", 63, on_done);
    redraw();
    LS_CHECK(tap_label("123"));
    LS_CHECK(tap_label("1/2"));
    const char *symbols = "[]{}<>\\^|`";
    for (const char *p = symbols; *p; p++) tap_key(*p);
    LS_CHECK(tap_label("2/2"));
    tap_key('~');
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_CHECK_MSG(!strcmp(s_got, "[]{}<>\\^|`~"), "got '%s'", s_got);
}

LS_CASE(secret_cancel_does_not_submit_or_carry_text_to_next_entry)
{
    s_done_calls = 0;
    ls_keyboard_open_secret("PASSWORD", 63, on_done);
    ls_keyboard_key(LS_TK_CHAR, 'x');
    redraw();
    LS_CHECK(tap_label("CANCEL"));
    LS_EQ_INT(s_done_calls, 0);
    ls_keyboard_open_secret("PASSWORD", 63, on_done);
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_EQ_INT(s_done_calls, 1);
    LS_CHECK(!s_got[0]);
}

static bool has_text(const char *text)
{
    for (int y = 0; y < H; ++y) {
        char row[W + 1]; row_text(y, row, sizeof(row));
        if (strstr(row, text)) return true;
    }
    return false;
}

LS_CASE(password_visibility_is_optional_and_resets_on_open)
{
    s_done_calls = 0;
    ls_keyboard_open_secret("PASSWORD", 63, on_done);
    const char *sample = "Demo123!";
    for (const char *p = sample; *p; ++p) ls_keyboard_key(LS_TK_CHAR, *p);
    redraw();
    LS_CHECK(!has_text(sample));
    LS_CHECK(tap_label("SHOW PASSWORD"));
    LS_CHECK(has_text(sample));
    LS_CHECK(tap_label("HIDE PASSWORD"));
    LS_CHECK(!has_text(sample));
    LS_CHECK(tap_label("SHOW PASSWORD"));
    LS_CHECK(tap_label("OK"));
    LS_EQ_INT(s_done_calls, 1);
    LS_CHECK(!strcmp(s_got, sample));
    ls_keyboard_open_secret("PASSWORD", 63, on_done);
    redraw();
    LS_CHECK(has_text("SHOW PASSWORD"));
    LS_CHECK(!has_text(sample));
    LS_CHECK(tap_label("CANCEL"));
    LS_EQ_INT(s_done_calls, 1);
    open_with("");
    LS_CHECK(!has_text("SHOW PASSWORD"));
}

LS_CASE(gutter_taps_type_once_and_pressed_color_recovers)
{
    open_with("x");
    int qx = -1, qy = -1, wx = -1, wy = -1;
    LS_CHECK(find_key('q', &qx, &qy));
    LS_CHECK(find_key('w', &wx, &wy));
    if (qx < 0 || wx < 0) return;
    const int gap = (qx + wx) / 2;
    LS_EQ_INT(g_back[qy * W + gap].ch, ' ');
    const uint8_t before = g_back[(qy - 1) * W + qx].attr;
    ls_keyboard_touch(gap, qy);
    redraw();
    LS_CHECK(g_back[(qy - 1) * W + qx].attr != before);
    ls_keyboard_touch(gap, qy);
    redraw();
    for (int i = 0; i < 16; ++i) redraw();
    LS_EQ_INT(g_back[(qy - 1) * W + qx].attr, before);
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_CHECK(!strcmp(s_got, "xqq"));
}

LS_CASE(letters_are_centred_on_the_p4_grid)
{
    static tui_cell back[54 * 71], front[54 * 71];
    tui_surface sf;
    tui_surface_setup(&sf, back, front, 54, 71);
    tui_frame_begin(&sf);
    ls_keyboard_open_secret("PASSWORD", 63, on_done);
    ls_keyboard_draw(&sf, tui_rect_make(0, 0, 54, 71));
    int found = 0;
    for (int y = 1; y < 71; ++y) {
        for (int x = 0; x < 54; ++x) {
            if (back[y * 54 + x].ch != 'q') continue;
            const uint8_t tile = back[(y - 1) * 54 + x].attr;
            int left = x, right = x;
            while (left > 0 && back[(y - 1) * 54 + left - 1].attr == tile) --left;
            while (right < 53 && back[(y - 1) * 54 + right + 1].attr == tile) ++right;
            LS_EQ_INT(x - left, right - x);
            found++;
        }
    }
    LS_EQ_INT(found, 1);
    ls_keyboard_close();
}

LS_CASE(secret_keyboard_fits_landscape)
{
    static tui_cell back[120 * 27], front[120 * 27];
    tui_surface sf;
    tui_surface_setup(&sf, back, front, 120, 27);
    tui_frame_begin(&sf);
    ls_keyboard_open_secret("PASSWORD", 63, on_done);
    ls_keyboard_draw(&sf, tui_rect_make(0, 0, 120, 27));
    bool show = false, ok = false, cancel = false;
    for (int y = 0; y < 27; ++y) {
        char row[121];
        for (int x = 0; x < 120; ++x) row[x] = back[y * 120 + x].ch;
        row[120] = 0;
        show |= strstr(row, "SHOW PASSWORD") != NULL;
        ok |= strstr(row, "OK") != NULL;
        cancel |= strstr(row, "CANCEL") != NULL;
    }
    LS_CHECK(show && ok && cancel);
    ls_keyboard_close();
}

LS_CASE(every_letter_has_a_figure_of_its_own)
{
    /* M and W both want five columns in a three column box, and the cheap
       way out - collapsing the diagonal - can make M read as N or W as U.
       The keyboard is unusable if two keys look the same, so every pair of
       letters has to differ somewhere. */
    static tui_cell b[W * H], f[W * H];
    tui_surface sf;
    char shot[26][LS_GLYPH_ROWS * LS_GLYPH_COLS + 1];

    for (int i = 0; i < 26; i++) {
        tui_surface_setup(&sf, b, f, W, H);
        tui_frame_begin(&sf);
        ls_glyph_draw(&sf, tui_rect_make(0, 0, LS_GLYPH_COLS, LS_GLYPH_ROWS),
                      (char)('A' + i), TUI_ATTR(TUI_WHITE, TUI_BLACK));
        int k = 0;
        for (int y = 0; y < LS_GLYPH_ROWS; y++)
            for (int x = 0; x < LS_GLYPH_COLS; x++)
                shot[i][k++] = (b[y * W + x].ch == LS_TUI_SHADE_FULL) ? 'X' : '.';
        shot[i][k] = 0;
        LS_CHECK_MSG(strchr(shot[i], 'X') != NULL,
                     "'%c' drew nothing at all", 'A' + i);
    }
    for (int i = 0; i < 26; i++)
        for (int j = i + 1; j < 26; j++)
            LS_CHECK_MSG(strcmp(shot[i], shot[j]) != 0,
                         "'%c' and '%c' draw the same figure: %s",
                         'A' + i, 'A' + j, shot[i]);
}
