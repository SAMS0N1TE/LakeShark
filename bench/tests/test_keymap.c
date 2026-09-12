/* LS_TEST_SOURCES: ${FW}/components/apps/shell/ls_keymap.c */
#include "ls_test.h"
#include "shell/ls_keymap.h"

LS_CASE(navigation_keys_map_to_focus_actions)
{
    LS_EQ_INT(LS_KEY_ACTION_NEXT, ls_keymap_action('\t'));
    LS_EQ_INT(LS_KEY_ACTION_PREV, ls_keymap_action(LS_KEY_SHIFT_TAB));
    LS_EQ_INT(LS_KEY_ACTION_PREV, ls_keymap_action(LS_KEY_UP));
    LS_EQ_INT(LS_KEY_ACTION_NEXT, ls_keymap_action(LS_KEY_DOWN));
    LS_EQ_INT(LS_KEY_ACTION_LEFT, ls_keymap_action(LS_KEY_LEFT));
    LS_EQ_INT(LS_KEY_ACTION_RIGHT, ls_keymap_action(LS_KEY_RIGHT));
}

LS_CASE(editing_keys_map_to_lvgl_actions)
{
    LS_EQ_INT(LS_KEY_ACTION_ENTER, ls_keymap_action('\r'));
    LS_EQ_INT(LS_KEY_ACTION_ENTER, ls_keymap_action('\n'));
    LS_EQ_INT(LS_KEY_ACTION_BACKSPACE, ls_keymap_action('\b'));
    LS_EQ_INT(LS_KEY_ACTION_DELETE, ls_keymap_action(0x7f));
    LS_EQ_INT(LS_KEY_ACTION_ESCAPE, ls_keymap_action(0x1b));
    LS_EQ_INT(LS_KEY_ACTION_HOME, ls_keymap_action(LS_KEY_HOME));
    LS_EQ_INT(LS_KEY_ACTION_END, ls_keymap_action(LS_KEY_END));
}

LS_CASE(printable_ascii_passes_through_and_controls_do_not)
{
    LS_EQ_INT(' ', ls_keymap_action(' '));
    LS_EQ_INT('A', ls_keymap_action('A'));
    LS_EQ_INT('~', ls_keymap_action('~'));
    LS_EQ_INT(LS_KEY_ACTION_NONE, ls_keymap_action(0));
    LS_EQ_INT(LS_KEY_ACTION_NONE, ls_keymap_action(0x1f));
    LS_EQ_INT(LS_KEY_ACTION_NONE, ls_keymap_action(0x80));
    LS_EQ_INT(LS_KEY_ACTION_NONE, ls_keymap_action(0xffff));
}
