/* Component-safe handle on the radio head links. */

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

/* False when the action is not wired on this build. */
bool ls_link_ctl_can_reboot(void);
void ls_link_ctl_reboot(void);
bool ls_link_ctl_can_reset_sdr(void);
void ls_link_ctl_reset_sdr(void);

#ifdef __cplusplus
}
#endif

#endif
