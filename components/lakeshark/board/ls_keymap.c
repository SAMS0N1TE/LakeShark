/* See ls_keymap.h for the source of this table and why it is copied. */
#include "ls_keymap.h"

#include <ctype.h>
#include <stddef.h>

#define CH(c, s)   { LS_KEY_CHAR, (c), (s) }
#define CH1(c)     { LS_KEY_CHAR, (c), 0 }
#define K(k)       { LS_KEY_##k, 0, 0 }

/* Rows of ten, in matrix order. Blank entries are positions the scan visits
   because the matrix is ten wide, not keys that exist. */
static const ls_keymap_entry_t MAP[7][10] = {
    /* row 0 */ { K(F1), K(F2), K(F3), K(F4), K(F5),
                  K(F6), K(F7), K(F8), K(F9), K(F10) },
    /* row 1 */ { K(ESC), K(ESC),
                  CH('1','!'), CH('2','@'), CH('3','#'), CH('4','$'),
                  CH('5','%'), CH('6','^'), CH('7','&'), CH('8','*') },
    /* row 2 */ { CH('q','\''), CH('w','_'), CH('e','-'), CH('r','+'),
                  CH('t','='),  CH('y','\\'), CH('u','|'), CH('i',';'),
                  CH('o',':'),  CH('p','"') },
    /* row 3 */ { K(CAPS),
                  CH('a','~'), CH('s','['), CH('d',']'), CH('f','{'),
                  CH('g','}'), CH('h',','), CH('j','`'), CH('k','/'),
                  CH('l','?') },
    /* row 4 */ { K(ALT),
                  CH1('z'), CH1('x'), CH1('c'), CH1('v'),
                  CH('b','.'), CH('n','<'), CH('m','>'),
                  K(CTRL), K(UP) },
    /* row 5 */ { K(FN), K(META), K(SHIFT), K(TAB),
                  CH(' ',' '), CH(' ',' '), CH(' ',' '),
                  K(FN), K(LEFT), K(DOWN) },
    /* row 6 */ { K(F11), CH('9','('), K(BACKSPACE), K(ENTER),
                  K(RECORD), K(ENTER), CH('0',')'), K(RIGHT),
                  K(NONE), K(NONE) },
};

const ls_keymap_entry_t *ls_keymap_lookup(uint8_t row, uint8_t col)
{
    if (row >= 7 || col >= 10) return NULL;
    const ls_keymap_entry_t *e = &MAP[row][col];
    return e->key == LS_KEY_NONE ? NULL : e;
}

char ls_keymap_char(const ls_keymap_entry_t *entry, bool shift, bool caps)
{
    if (!entry || entry->key != LS_KEY_CHAR) return 0;
    /* Caps is a letter lock, not a second shift: it must not turn 3 into #.
       Shift does both, because that is how the vendor table pairs them. */
    if (shift && entry->shifted) return entry->shifted;
    if (caps && entry->ch >= 'a' && entry->ch <= 'z')
        return (char)toupper((unsigned char)entry->ch);
    return entry->ch;
}

bool ls_keymap_repeats(const ls_keymap_entry_t *entry)
{
    if (!entry) return false;
    switch (entry->key) {
    /* Typing and moving: holding these is how a person deletes a line or
       scrolls a list, and each event adds to the last. */
    case LS_KEY_CHAR:
    case LS_KEY_BACKSPACE:
    case LS_KEY_UP:
    case LS_KEY_DOWN:
    case LS_KEY_LEFT:
    case LS_KEY_RIGHT:
        return true;
    /* Everything else changes a mode or a place. A second one does not mean
       more of the same thing, it means it happened twice. */
    default:
        return false;
    }
}

const char *ls_keymap_name(const ls_keymap_entry_t *entry)
{
    if (!entry) return "-";
    switch (entry->key) {
    case LS_KEY_CHAR:      return "char";
    case LS_KEY_ESC:       return "ESC";
    case LS_KEY_TAB:       return "TAB";
    case LS_KEY_ENTER:     return "ENTER";
    case LS_KEY_BACKSPACE: return "BKSP";
    case LS_KEY_UP:        return "UP";
    case LS_KEY_DOWN:      return "DOWN";
    case LS_KEY_LEFT:      return "LEFT";
    case LS_KEY_RIGHT:     return "RIGHT";
    case LS_KEY_SHIFT:     return "SHIFT";
    case LS_KEY_CTRL:      return "CTRL";
    case LS_KEY_ALT:       return "ALT";
    case LS_KEY_META:      return "META";
    case LS_KEY_FN:        return "FN";
    case LS_KEY_CAPS:      return "CAPS";
    case LS_KEY_RECORD:    return "REC";
    case LS_KEY_F1:  return "F1";  case LS_KEY_F2:  return "F2";
    case LS_KEY_F3:  return "F3";  case LS_KEY_F4:  return "F4";
    case LS_KEY_F5:  return "F5";  case LS_KEY_F6:  return "F6";
    case LS_KEY_F7:  return "F7";  case LS_KEY_F8:  return "F8";
    case LS_KEY_F9:  return "F9";  case LS_KEY_F10: return "F10";
    case LS_KEY_F11: return "F11";
    default:               return "-";
    }
}
