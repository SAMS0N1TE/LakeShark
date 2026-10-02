/* ls_radio_select asks the board through here. FIND's sources ask
   ls_rsel_absent in turn (ls_df_sources.c), so a radio is fitted or not in
   the same words in every app. */
#include "ls_radio_select.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "ls_lora.h"
#include "ls_mixrf.h"
#include "ls_tui_screen.h"
#include "ls_wireless.h"
#include "link_ctl.h"
#include "radio/radio_endpoint.h"

/* Whether the endpoint is plugged in, and who holds it. */
static bool endpoint_present(const char *id, char *owner, size_t cap)
{
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_get(id, &info) != LS_RADIO_OK || !info.present) return false;
    if (info.leased) snprintf(owner, cap, "%s", info.owner);
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
                                                     out->sdr_owner[0], sizeof(out->sdr_owner[0]));
    out->present[LS_RSEL_SDR_HACKRF] = endpoint_present(LS_RADIO_ENDPOINT_HACKRF_USB,
                                                        out->sdr_owner[1], sizeof(out->sdr_owner[1]));
    out->present[LS_RSEL_LORA] = ls_lora_present();
    out->present[LS_RSEL_CC1101] = m.cc;
    out->present[LS_RSEL_NRF24] = m.nrf;
    out->present[LS_RSEL_NFC] = m.nfc;
    out->mixrf_unknown = !m.ready || m.busy;
    out->present[LS_RSEL_WIFI] = ws.wifi_available;
    out->present[LS_RSEL_BLE] = ws.bt_available;
    /* On the board, on its own UART: whether it is switched on is the GPS
       app's business, not whether it is there. */
    out->present[LS_RSEL_GPS] = true;
    out->lora_caps = ls_lora_caps();
    out->lora_lr20xx = ls_lora_chip() == LS_LORA_CHIP_LR20XX;
    out->lora_name = ls_lora_chip_name();
}

void ls_rsel_hw_restart(void)
{
    /* Only a receiver that is running has an endpoint to give back; one
       that starts later acquires the new choice by itself. */
    if (ls_tui_radio_claimed()) ls_link_ctl_reset_sdr();
}
