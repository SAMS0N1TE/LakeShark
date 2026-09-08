#include "usb_autoreboot_pref.h"

/*LS-770*/
/* The persist callback is only fired on an actual value change: the toggle
   callback would otherwise queue an NVS write on every timer redraw that
   re-labels the row (see AppSettings::timerCb / AppP25::updateSettings).
   Init writes the loaded value straight through without persisting so the
   very first boot after a factory erase does not commit its own default. */

static bool s_value;
static usb_autoreboot_persist_fn s_persist;

void usb_autoreboot_pref_init(usb_autoreboot_persist_fn persist, bool initial)
{
    s_value   = initial;
    s_persist = persist;
}

void usb_autoreboot_pref_set(bool en)
{
    if (s_value == en) return;
    s_value = en;
    if (s_persist) s_persist(en);
}

bool usb_autoreboot_pref_get(void) { return s_value; }
