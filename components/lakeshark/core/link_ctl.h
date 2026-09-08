/* LS-785  Component-safe handle on the radio head links.
 *
 * BLE lives in main/ (ble_link.c) and the GUI apps live in components/apps,
 * which cannot depend on main. The Settings screen still needs to turn
 * Bluetooth off - the head link can sit in a reconnect loop when the head
 * refuses pairing, and until now the only way to stop it was the serial
 * console, which is exactly the thing a handheld does not have.
 *
 * main registers the implementation at startup; before that, and on builds
 * with no BLE at all, every call is a safe no-op and ls_link_ctl_available()
 * reports false so the UI can disable the control rather than lie about it.
 */
#ifndef LS_LINK_CTL_H
#define LS_LINK_CTL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*ble_enable)(bool on);
    bool (*ble_is_on)(void);
    bool (*ble_is_connected)(void);
    /*LS-800  Recovery actions the operator needs without a serial console:
       the receiver stalls, or an app refuses to load on low memory, and the
       only way out today is a cable. Any of these may be NULL. */
    void (*reboot)(void);
    void (*sdr_reset)(void);
} ls_link_ctl_t;

/* Called once from main. Passing NULL unregisters. */
void ls_link_ctl_register(const ls_link_ctl_t *ctl);

/* False when no implementation is registered (headless/no-BLE builds). */
bool ls_link_ctl_available(void);

void ls_link_ctl_set_ble(bool on);
bool ls_link_ctl_ble_on(void);
bool ls_link_ctl_ble_connected(void);

/*LS-800  False when the action is not wired on this build. */
bool ls_link_ctl_can_reboot(void);
void ls_link_ctl_reboot(void);
bool ls_link_ctl_can_reset_sdr(void);
void ls_link_ctl_reset_sdr(void);

#ifdef __cplusplus
}
#endif

#endif
