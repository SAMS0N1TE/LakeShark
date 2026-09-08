#ifndef LS_KEYMAP_H
#define LS_KEYMAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Printable keys use their ASCII value.  Controller drivers translate their
   non-printing matrix positions to this small, board-independent namespace. */
typedef enum {
    LS_KEY_UP = 0x100,
    LS_KEY_DOWN,
    LS_KEY_RIGHT,
    LS_KEY_LEFT,
    LS_KEY_SHIFT_TAB,
    LS_KEY_HOME,
    LS_KEY_END,
} ls_key_code_t;

/* Values deliberately match LVGL 8's lv_key_t.  Keeping the pure mapping free
   of LVGL makes it host-testable and keeps board drivers independent of GUI
   headers. */
typedef enum {
    LS_KEY_ACTION_NONE      = 0,
    LS_KEY_ACTION_HOME      = 2,
    LS_KEY_ACTION_END       = 3,
    LS_KEY_ACTION_BACKSPACE = 8,
    LS_KEY_ACTION_NEXT      = 9,
    LS_KEY_ACTION_ENTER     = 10,
    LS_KEY_ACTION_PREV      = 11,
    LS_KEY_ACTION_RIGHT     = 19,
    LS_KEY_ACTION_LEFT      = 20,
    LS_KEY_ACTION_ESCAPE    = 27,
    LS_KEY_ACTION_DELETE    = 127,
} ls_key_action_t;

uint32_t ls_keymap_action(uint32_t key);

#ifdef __cplusplus
}
#endif

#endif
