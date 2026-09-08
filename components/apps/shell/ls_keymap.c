#include "shell/ls_keymap.h"

uint32_t ls_keymap_action(uint32_t key)
{
    switch (key) {
    case '\t':             return LS_KEY_ACTION_NEXT;
    case '\r':
    case '\n':             return LS_KEY_ACTION_ENTER;
    case '\b':             return LS_KEY_ACTION_BACKSPACE;
    case 0x1b:             return LS_KEY_ACTION_ESCAPE;
    case 0x7f:             return LS_KEY_ACTION_DELETE;
    case LS_KEY_SHIFT_TAB: return LS_KEY_ACTION_PREV;
    /* Up/down traverse the focus group.  Left/right remain widget actions so
       a focused slider, table or text cursor can still be adjusted. */
    case LS_KEY_UP:        return LS_KEY_ACTION_PREV;
    case LS_KEY_DOWN:      return LS_KEY_ACTION_NEXT;
    case LS_KEY_LEFT:      return LS_KEY_ACTION_LEFT;
    case LS_KEY_RIGHT:     return LS_KEY_ACTION_RIGHT;
    case LS_KEY_HOME:      return LS_KEY_ACTION_HOME;
    case LS_KEY_END:       return LS_KEY_ACTION_END;
    default:
        return (key >= 0x20 && key <= 0x7e) ? key : LS_KEY_ACTION_NONE;
    }
}
