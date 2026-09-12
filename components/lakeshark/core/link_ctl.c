/* See link_ctl.h. */
#include "link_ctl.h"

#include <stddef.h>

static ls_link_ctl_t s_ctl;
static bool          s_have;

void ls_link_ctl_register(const ls_link_ctl_t *ctl)
{
    if (!ctl) {
        s_have = false;
        return;
    }
    s_ctl  = *ctl;
    s_have = true;
}

bool ls_link_ctl_available(void)
{
    return s_have && s_ctl.ble_enable && s_ctl.ble_is_on;
}

void ls_link_ctl_set_ble(bool on)
{
    if (ls_link_ctl_available()) s_ctl.ble_enable(on);
}

bool ls_link_ctl_ble_on(void)
{
    return ls_link_ctl_available() ? s_ctl.ble_is_on() : false;
}

bool ls_link_ctl_ble_connected(void)
{
    /* Connected is a stronger claim than on; never infer it from on. */
    return (s_have && s_ctl.ble_is_connected) ? s_ctl.ble_is_connected() : false;
}

/**/
bool ls_link_ctl_can_reboot(void) { return s_have && s_ctl.reboot; }
void ls_link_ctl_reboot(void)     { if (ls_link_ctl_can_reboot()) s_ctl.reboot(); }

bool ls_link_ctl_can_reset_sdr(void) { return s_have && s_ctl.sdr_reset; }
void ls_link_ctl_reset_sdr(void)     { if (ls_link_ctl_can_reset_sdr()) s_ctl.sdr_reset(); }
