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
    char     shifted;   /* Fn symbol column; 0 when the key has no symbol */
} ls_keymap_entry_t;

/* Look up a matrix position. Returns NULL for positions with no key on them,
   which a 10-wide scan of a keyboard this shape does produce. */
const ls_keymap_entry_t *ls_keymap_lookup(uint8_t row, uint8_t col);

/* Resolve to a printable character given the live modifier state, or 0 when
   the key does not produce one. Shift XOR Caps capitalises letters. Fn
   selects the symbol column, with no fallback for keys without a symbol.
   Shift retains the symbol column for non-letters. */
char ls_keymap_char(const ls_keymap_entry_t *entry, bool shift, bool caps,
                    bool fn);

/* A modifier is active while held, or for one key after an unused tap.
   Zero-initialise before use. held is a bit per physical key, so either Fn
   key can be released without cancelling the other one. */
typedef struct {
    uint8_t held;
    bool pending;
    bool used;
} ls_keymap_modifier_t;

/* key_bit identifies a physical modifier key (one bit per matrix column).
   Releasing an unused tap arms it; tapping again cancels the pending tap. */
void ls_keymap_modifier_event(ls_keymap_modifier_t *mod, uint8_t key_bit,
                              bool pressed);

/* Call on every other key press, even a key that produces no character.
   Returns the state for that press and consumes a pending one-shot tap.
   Key releases do not consume it; held modifiers survive key repeats. */
bool ls_keymap_modifier_use(ls_keymap_modifier_t *mod);

/* Short human name for a key, for logs and the console. Never NULL. */
const char *ls_keymap_name(const ls_keymap_entry_t *entry);

bool ls_keymap_repeats(const ls_keymap_entry_t *entry);

#ifdef __cplusplus
}
#endif

#endif /* LS_KEYMAP_H */
