#ifndef USB_AUTOREBOOT_PREF_H
#define USB_AUTOREBOOT_PREF_H

/**/
/* USB AUTO-REBOOT lived as a process-static bool in app_registry with no
   read-back from NVS at boot, so the Settings and P25 CONFIG toggles
   presented ON among persistent device settings but silently reverted to
   OFF on every power cycle.  The state lives in its own module now so the
   set/persist/reload contract can be pinned by the bench without pulling
   in nvs.h. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*usb_autoreboot_persist_fn)(bool en);

void usb_autoreboot_pref_init(usb_autoreboot_persist_fn persist, bool initial);
void usb_autoreboot_pref_set(bool en);
bool usb_autoreboot_pref_get(void);

#ifdef __cplusplus
}
#endif

#endif
