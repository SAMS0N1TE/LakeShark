/* LS_TEST_SOURCES: ${FW}/components/lakeshark/board/ls_keymap.c */

#include "ls_test.h"
#include "ls_keymap.h"

#include <string.h>

#define ROWS 7
#define COLS 10

/* ---------------------------------------------------------------- bounds -- */

LS_CASE(the_lookup_rejects_positions_outside_the_matrix)
{
    LS_CHECK(ls_keymap_lookup(ROWS, 0) == NULL);
    LS_CHECK(ls_keymap_lookup(0, COLS) == NULL);
    LS_CHECK(ls_keymap_lookup(ROWS, COLS) == NULL);
    LS_CHECK(ls_keymap_lookup(255, 255) == NULL);

    LS_CHECK(ls_keymap_lookup(6, 8) == NULL);
    LS_CHECK(ls_keymap_lookup(6, 9) == NULL);
}

LS_CASE(every_position_either_names_a_key_or_is_empty)
{
    /* A key whose name falls through to the default would print as an
       unknown in the diagnostics, which is how a wrong enum value hides. */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const ls_keymap_entry_t *e = ls_keymap_lookup((uint8_t)r, (uint8_t)c);
            if (!e) continue;
            const char *n = ls_keymap_name(e);
            LS_CHECK_MSG(n && n[0] && strcmp(n, "?") != 0,
                         "row %d col %d has no name", r, c);
        }
    LS_CHECK_MSG(strcmp(ls_keymap_name(NULL), "-") == 0,
                 "a null entry should name itself as absent");
}

/* --------------------------------------------------------- reachability -- */

/* Can this character be produced from anywhere on the matrix, in any of the
   three modifier states the firmware actually uses? */
static bool can_type(char want)
{
    static const struct { bool shift, caps; } MODES[] = {
        { false, false }, { true, false }, { false, true }, { true, true },
    };
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const ls_keymap_entry_t *e = ls_keymap_lookup((uint8_t)r, (uint8_t)c);
            if (!e) continue;
            for (unsigned m = 0; m < sizeof(MODES) / sizeof(MODES[0]); m++)
                if (ls_keymap_char(e, MODES[m].shift, MODES[m].caps) == want)
                    return true;
        }
    return false;
}

LS_CASE(every_printable_character_can_be_typed)
{

    for (char ch = 0x20; ch > 0 && ch <= 0x7E; ch++)
        LS_CHECK_MSG(can_type(ch), "'%c' (0x%02X) cannot be typed",
                     ch, (unsigned char)ch);
}

LS_CASE(the_characters_a_frequency_needs_are_all_there)
{

    static const char NEEDED[] = "0123456789.-/: ";
    for (const char *c = NEEDED; *c; c++)
        LS_CHECK_MSG(can_type(*c), "cannot type '%c'", *c);
}

LS_CASE(all_twenty_six_letters_are_present_in_both_cases)
{
    for (char c = 'a'; c <= 'z'; c++) {
        LS_CHECK_MSG(can_type(c), "no lowercase '%c'", c);
        LS_CHECK_MSG(can_type((char)(c - 32)), "no uppercase '%c'", c - 32);
    }
}

/* --------------------------------------------------- modifier behaviour -- */

/* Find the entry whose unshifted character is `ch`. */
static const ls_keymap_entry_t *entry_for(char ch)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const ls_keymap_entry_t *e = ls_keymap_lookup((uint8_t)r, (uint8_t)c);
            if (e && e->key == LS_KEY_CHAR && e->ch == ch) return e;
        }
    return NULL;
}

LS_CASE(caps_is_a_letter_lock_and_leaves_everything_else_alone)
{
    /* The documented invariant: caps must not turn 3 into #. That is what
       shift is for, and conflating them makes the number row unusable with
       caps on - which is exactly when somebody is typing a callsign. */
    for (char d = '0'; d <= '9'; d++) {
        const ls_keymap_entry_t *e = entry_for(d);
        LS_CHECK_MSG(e != NULL, "digit '%c' is not on the matrix", d);
        LS_CHECK_MSG(ls_keymap_char(e, false, true) == d,
                     "caps changed '%c'", d);
    }
}

LS_CASE(caps_uppercases_every_letter)
{
    for (char c = 'a'; c <= 'z'; c++) {
        const ls_keymap_entry_t *e = entry_for(c);
        LS_CHECK_MSG(e != NULL, "letter '%c' is not on the matrix", c);
        LS_CHECK_MSG(ls_keymap_char(e, false, true) == (char)(c - 32),
                     "caps did not uppercase '%c'", c);
    }
}

LS_CASE(shift_is_the_symbol_layer_and_wins_over_caps)
{

    const ls_keymap_entry_t *q = entry_for('q');
    LS_CHECK(q != NULL);
    LS_EQ_INT('\'', ls_keymap_char(q, true, false));
    LS_EQ_INT('\'', ls_keymap_char(q, true, true));
    LS_EQ_INT('Q',  ls_keymap_char(q, false, true));
    LS_EQ_INT('q',  ls_keymap_char(q, false, false));
}

LS_CASE(a_letter_with_no_symbol_is_unchanged_by_shift)
{

    static const char BARE[] = "zxcv";
    for (const char *c = BARE; *c; c++) {
        const ls_keymap_entry_t *e = entry_for(*c);
        LS_CHECK_MSG(e != NULL, "'%c' is not on the matrix", *c);
        LS_CHECK_MSG(ls_keymap_char(e, true, false) == *c,
                     "shift changed '%c' to '%c'", *c,
                     ls_keymap_char(e, true, false));
        /* Still reachable in uppercase through caps. */
        LS_CHECK(ls_keymap_char(e, false, true) == (char)(*c - 32));
    }
}

LS_CASE(a_key_that_is_not_a_character_yields_no_character)
{
    /* The scan feeds every entry through ls_keymap_char, so a modifier or an
       arrow returning a stray byte would be typed into whatever has focus. */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const ls_keymap_entry_t *e = ls_keymap_lookup((uint8_t)r, (uint8_t)c);
            if (!e || e->key == LS_KEY_CHAR) continue;
            for (int m = 0; m < 4; m++)
                LS_CHECK_MSG(ls_keymap_char(e, m & 1, (m >> 1) & 1) == 0,
                             "%s at row %d col %d produced a character",
                             ls_keymap_name(e), r, c);
        }
    LS_EQ_INT(0, ls_keymap_char(NULL, false, false));
}

LS_CASE(the_modifiers_the_firmware_relies_on_all_exist)
{
    /* compact_ui tracks shift and caps and the router needs the function
       keys. A modifier missing from the transcription disables a whole layer
       silently. */
    static const ls_key_t NEEDED[] = {
        LS_KEY_SHIFT, LS_KEY_CAPS, LS_KEY_CTRL, LS_KEY_ALT, LS_KEY_FN,
        LS_KEY_ENTER, LS_KEY_ESC, LS_KEY_TAB, LS_KEY_BACKSPACE,
        LS_KEY_UP, LS_KEY_DOWN, LS_KEY_LEFT, LS_KEY_RIGHT,
        LS_KEY_F1, LS_KEY_F5, LS_KEY_F10, LS_KEY_F11,
    };
    for (unsigned i = 0; i < sizeof(NEEDED) / sizeof(NEEDED[0]); i++) {
        bool found = false;
        for (int r = 0; r < ROWS && !found; r++)
            for (int c = 0; c < COLS && !found; c++) {
                const ls_keymap_entry_t *e =
                    ls_keymap_lookup((uint8_t)r, (uint8_t)c);
                if (e && e->key == NEEDED[i]) found = true;
            }
        LS_CHECK_MSG(found, "key %d is not on the matrix", (int)NEEDED[i]);
    }
}

/* ------------------------------------------------------------- repeating -- */

/* Auto-repeat policy. The keypad driver arms its repeat timer from this, and
   the failure it prevents is specific: the repeat generator emits synthetic
   press events, and CAPS toggles its flag on every press. Holding CAPS
   therefore flipped caps about twenty-two times a second and settled wherever
   the release happened to fall. F9 cycles the theme and F11 turns the screen,
   which restarts the TUI session; both were repeating too. */

LS_CASE(typing_and_moving_keys_repeat)
{
    /* Holding backspace to clear a field and holding an arrow to scroll are
       the two gestures this exists for. */
    LS_CHECK(ls_keymap_repeats(entry_for('a')));
    LS_CHECK(ls_keymap_repeats(entry_for('0')));
    LS_CHECK(ls_keymap_repeats(entry_for(' ')));

    static const ls_key_t MOVERS[] = {
        LS_KEY_BACKSPACE, LS_KEY_UP, LS_KEY_DOWN, LS_KEY_LEFT, LS_KEY_RIGHT,
    };
    for (unsigned i = 0; i < sizeof(MOVERS) / sizeof(MOVERS[0]); i++) {
        bool found = false;
        for (int r = 0; r < ROWS && !found; r++)
            for (int c = 0; c < COLS && !found; c++) {
                const ls_keymap_entry_t *e =
                    ls_keymap_lookup((uint8_t)r, (uint8_t)c);
                if (e && e->key == MOVERS[i]) {
                    LS_CHECK_MSG(ls_keymap_repeats(e),
                                 "%s should repeat", ls_keymap_name(e));
                    found = true;
                }
            }
        LS_CHECK(found);
    }
}

LS_CASE(nothing_that_toggles_or_transitions_repeats)
{

    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const ls_keymap_entry_t *e = ls_keymap_lookup((uint8_t)r, (uint8_t)c);
            if (!e) continue;
            switch (e->key) {
            case LS_KEY_CHAR: case LS_KEY_BACKSPACE:
            case LS_KEY_UP: case LS_KEY_DOWN:
            case LS_KEY_LEFT: case LS_KEY_RIGHT:
                continue;
            default:
                LS_CHECK_MSG(!ls_keymap_repeats(e),
                             "%s at row %d col %d repeats", ls_keymap_name(e),
                             r, c);
            }
        }
}

LS_CASE(caps_and_the_function_keys_specifically_do_not_repeat)
{

    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const ls_keymap_entry_t *e = ls_keymap_lookup((uint8_t)r, (uint8_t)c);
            if (!e) continue;
            if (e->key == LS_KEY_CAPS)
                LS_CHECK_MSG(!ls_keymap_repeats(e), "CAPS repeats");
            if (e->key == LS_KEY_F9)
                LS_CHECK_MSG(!ls_keymap_repeats(e), "F9 repeats, cycling themes");
            if (e->key == LS_KEY_F11)
                LS_CHECK_MSG(!ls_keymap_repeats(e), "F11 repeats, rotating");
            if (e->key == LS_KEY_SHIFT)
                LS_CHECK_MSG(!ls_keymap_repeats(e), "SHIFT repeats");
            if (e->key == LS_KEY_ENTER)
                LS_CHECK_MSG(!ls_keymap_repeats(e), "ENTER repeats");
        }
}

LS_CASE(a_null_entry_does_not_repeat)
{
    /* The driver looks the key up before asking, and an unmapped position
       returns NULL. Arming a repeat on that would fire a key that is not
       there. */
    LS_CHECK(!ls_keymap_repeats(NULL));
}
