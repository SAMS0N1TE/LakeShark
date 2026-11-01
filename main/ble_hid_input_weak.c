/* Standalone bridge until the common input queue supplies its strong symbol.
 * The adapter always calls ls_input_post, including pointer and release edges. */
#include "tui/ls_input_ev.h"
extern bool compact_ui_inject_key(uint16_t key, char ch) __attribute__((weak));
__attribute__((weak)) bool ls_input_post(const ls_input_ev_t *ev)
{
    if (!ev)
        return false;
    if (ev->kind == LS_IEV_KEY_DOWN && compact_ui_inject_key)
        return compact_ui_inject_key(ev->key, ev->ch);
    return true;
}
