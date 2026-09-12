/* T-Display-P4 keyboard layout: matrix position to key. */

#ifndef LS_KEYMAP_H
#define LS_KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_KEY_NONE = 0,
    LS_KEY_CHAR,        /* ch / shifted carry the character */
    LS_KEY_ESC,
    LS_KEY_TAB,
    LS_KEY_ENTER,
    LS_KEY_BACKSPACE,
    LS_KEY_UP,
    LS_KEY_DOWN,
    LS_KEY_LEFT,
    LS_KEY_RIGHT,
    LS_KEY_SHIFT,
    LS_KEY_CTRL,
    LS_KEY_ALT,
    LS_KEY_META,
    LS_KEY_FN,
    LS_KEY_CAPS,
    LS_KEY_RECORD,
    LS_KEY_F1, LS_KEY_F2, LS_KEY_F3, LS_KEY_F4, LS_KEY_F5, LS_KEY_F6,
    LS_KEY_F7, LS_KEY_F8, LS_KEY_F9, LS_KEY_F10, LS_KEY_F11,
} ls_key_t;

typedef struct {
    ls_key_t key;
    char     ch;        /* unshifted, 0 when the key is not a character */
    char     shifted;   /* 0 when the key has no shifted form */
} ls_keymap_entry_t;

/* Look up a matrix position. Returns NULL for positions with no key on them,
   which a 10-wide scan of a keyboard this shape does produce. */
const ls_keymap_entry_t *ls_keymap_lookup(uint8_t row, uint8_t col);

/* Resolve to a printable character given the live modifier state, or 0 when
   the key does not produce one. Caps affects letters only; shift affects
   both letters and the symbol row, the way the vendor table is written. */
char ls_keymap_char(const ls_keymap_entry_t *entry, bool shift, bool caps);

/* Short human name for a key, for logs and the console. Never NULL. */
const char *ls_keymap_name(const ls_keymap_entry_t *entry);

/* Whether holding this key should keep producing it.

   Auto-repeat is for keys whose effect accumulates: characters, backspace and
   the arrows. It is wrong for anything that toggles or transitions, because a
   repeat is indistinguishable from a fresh press to whatever consumes it.
   CAPS is the clearest case - it flips a flag on every press, so repeating it
   at 22 a second leaves the flag wherever the release happened to land. The
   function keys are as bad in their own way: one cycles the theme and another
   turns the screen, which tears the TUI session down and rebuilds it. */
bool ls_keymap_repeats(const ls_keymap_entry_t *entry);

#ifdef __cplusplus
}
#endif

#endif /* LS_KEYMAP_H */
