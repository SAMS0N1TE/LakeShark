/* ls_radio_select asks the board through here. FIND's sources ask
   ls_rsel_absent in turn (ls_df_sources.c), so a radio is fitted or not in
   the same words in every app. */
#include "ls_radio_select.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "ls_keypad.h"
#include "ls_lora.h"
#include "ls_mixrf.h"
#include "ls_tui_screen.h"
#include "ls_wireless.h"
#include "link_ctl.h"
#include "radio/radio_endpoint.h"

/* Whether the endpoint is plugged in, who holds it, and whether it has been
   at any point this boot. The registry keeps a slot for an endpoint that has
   attached once, present or not, so a slot is the answer to the last; a busy
   registry leaves what was last seen. */
static bool endpoint_present(const char *id, char *owner, size_t cap, bool *seen)
{
    static ls_radio_endpoint_info_t cached[2];
    static bool had[2];
    const int slot = !strcmp(id, LS_RADIO_ENDPOINT_HACKRF_USB);
    ls_radio_endpoint_info_t *info = &cached[slot];
    /* Radio owners may hold the registry while doing IO. Never wait in draw. */
    if (ls_radio_endpoint_try_get(id, info) == LS_RADIO_OK) had[slot] = true;
    *seen = had[slot] || info->present;
    if (!info->present) return false;
    if (info->leased) snprintf(owner, cap, "%s", info->owner);
    return true;
}

void ls_rsel_hw(ls_rsel_hw_t *out)
{
    /* PSRAM: both are larger than a UI stack wants to carry. */
    EXT_RAM_BSS_ATTR static ls_wireless_snapshot_t ws;
    EXT_RAM_BSS_ATTR static ls_mixrf_status_t m;
    memset(out, 0, sizeof(*out));
    ls_wireless_get(&ws);
    ls_mixrf_snapshot(&m);
    out->present[LS_RSEL_SDR_RTL] = endpoint_present(LS_RADIO_ENDPOINT_RTL_USB,
                                                     out->sdr_owner[0], sizeof(out->sdr_owner[0]),
                                                     &out->sdr_seen[0]);
    out->present[LS_RSEL_SDR_HACKRF] = endpoint_present(LS_RADIO_ENDPOINT_HACKRF_USB,
                                                        out->sdr_owner[1], sizeof(out->sdr_owner[1]),
                                                        &out->sdr_seen[1]);
    /* The socket's part, bound or parked by the mesh: whoever picks it
       starts it again (ls_lora.h). */
    out->present[LS_RSEL_LORA] = ls_lora_present() ||
                                 (ls_lora_fitted_caps() & LS_LORA_CAP_LORA);
    out->present[LS_RSEL_CC1101] = m.cc;
    out->present[LS_RSEL_NRF24] = m.nrf;
    out->present[LS_RSEL_NFC] = m.nfc;
    out->mixrf_unknown = !m.ready || m.busy;
    /* The keyboard board carries the CC1101, nRF24 and NFC. ls_keypad_present
       follows it on and off the bus without probing anything; a part that
       answered counts too. Sticky for the boot, so one that is mid-replug
       keeps its rows. */
    static bool had_mixrf;
    if (ls_keypad_present() || m.keyboard || m.cc || m.nrf || m.nfc) had_mixrf = true;
    out->mixrf_seen = had_mixrf;
    out->present[LS_RSEL_WIFI] = ws.wifi_available;
    out->present[LS_RSEL_BLE] = ws.bt_available;
    /* On the board, on its own UART: whether it is switched on is the GPS
       app's business, not whether it is there. */
    out->present[LS_RSEL_GPS] = true;
    out->lora_caps = ls_lora_present() ? ls_lora_caps() : ls_lora_fitted_caps();
    out->lora_lr20xx = ls_lora_fitted() == LS_LORA_CHIP_LR20XX;
    out->lora_name = ls_lora_fitted_name();
}

void ls_rsel_hw_restart(void)
{
    /* Only a receiver that is running has an endpoint to give back; one
       that starts later acquires the new choice by itself. */
    if (ls_tui_radio_claimed()) ls_link_ctl_reset_sdr();
}
