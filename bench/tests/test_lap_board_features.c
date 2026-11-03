/* Disabled services retain the ELF ABI and fail before taking any resource. */
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "ls_lap_net.h"
#include "ls_lap_radio.h"
#include "ls_lap_radio_io.h"
#include "ls_lap_net_audio.h"
#include "ls_lap_dsp.h"
#include "ls_lap_usage.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    extern bool ls_lap_radio_sim_hackrf;
    ls_lap_radio_sim_hackrf = true;
    static const char *exports[] = {
        "lap_ble_advertise", "lap_gatt_server_open", "lap_hid_device_open",
        "lap_tx_hackrf_config", "lap_tx_iq", "lap_tx_replay",
        "lap_rx_dsp_start", "lap_rx_dsp_status", "lap_rx_dsp_next", "lap_rx_dsp_close",
        "lap_nfc_open", "lap_nfc_next", "lap_nfc_write", "lap_nfc_close",
        "lap_mdns_browse", "lap_mdns_register", "lap_mdns_next", "lap_mdns_stop"
    };
    for (unsigned i=0; i<sizeof(exports)/sizeof(exports[0]); i++)
        assert(ls_lap_resolve(exports[i]));
    ls_lap_sys_host()->hardware = LAP_HW_WIFI | LAP_HW_BLE | LAP_HW_SD | LAP_HW_SPEAKER | LAP_HW_MIC;
    ls_lap_api_begin("board-features", 1, 0);
    ls_lap_sys_begin();
    lap_app_t app = {.api_major=2, .api_minor=6,
        .flags=LAP_CAP_NETWORK | LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX | LAP_CAP_MIC};
    strcpy(app.id, "board-features");
    ls_lap_radio_begin(&app);
    ls_lap_net_begin(true);
    ls_lap_api_callback(false);
    ls_lap_sys_service();
    ls_lap_radio_service();
    ls_lap_api_callback(true);

    lap_caps_t caps;
    assert(!lap_caps(&caps, sizeof(caps)));
    assert((caps.hardware & (LAP_HW_WIFI | LAP_HW_BLE | LAP_HW_SPEAKER | LAP_HW_MIC)) ==
           (LAP_HW_WIFI | LAP_HW_BLE | LAP_HW_SPEAKER | LAP_HW_MIC));
    assert(!(caps.hardware & (LAP_HW_BLE_ADVERTISER | LAP_HW_GATT_SERVER | LAP_HW_HID_DEVICE |
                             LAP_HW_NFC | LAP_HW_NDEF_WRITE | LAP_HW_WORKER_DSP | LAP_HW_MDNS)));
    for (unsigned i=0; i<caps.radio_count; i++)
        if (caps.radio_kind[i] == LAP_RADIO_KIND_HACKRF) assert(!(caps.radio_tx & (1u<<i)));
    assert(caps.radio_count == 1 && caps.radio_kind[0] == LAP_RADIO_KIND_HACKRF);
    lap_radio_modes_t modes;
    assert(!lap_radio_modes(0, &modes));
    assert(!(modes.modes & (LAP_MODE_TX_IQ | LAP_MODE_TX_PACKET | LAP_MODE_TX_EDGES)));
    lap_packet_config_t tx={.size=sizeof(tx), .version=1, .frequency_hz=100000000,
        .modulation=LAP_MOD_LORA, .sf=7, .coding_rate=5, .bandwidth_hz=125000};
    int8_t iq[2]={0};
    assert(lap_tx_iq(0, &tx, iq, 1, 2000000) == LAP_RX_EUNSUPPORTED);
    assert(lap_tx_replay(0, &tx, "fixture") == LAP_RX_EUNSUPPORTED);
    assert(lap_ble_advertise(NULL) == LAP_IO_ENOTSUP);
    assert(lap_gatt_server_open(NULL, 0) == LAP_IO_ENOTSUP);
    assert(lap_hid_device_open(LAP_HID_KEYBOARD, NULL) == LAP_IO_ENOTSUP);
    assert(lap_tx_hackrf_config(0, NULL) == LAP_RX_EUNSUPPORTED);
    assert(lap_rx_dsp_start(0, NULL) == LAP_RX_EUNSUPPORTED);
    assert(ls_lap_dsp_attach(0, NULL) == LAP_RX_EUNSUPPORTED);
    assert(lap_rx_dsp_status(0, NULL) == LAP_RX_EUNSUPPORTED);
    assert(lap_rx_dsp_next(0, NULL, NULL, 0) == LAP_RX_EUNSUPPORTED);
    assert(!ls_lap_dsp_service() && !ls_lap_dsp_on_worker());
    assert(!ls_lap_dsp_usage(0).bytes && !ls_lap_dsp_usage(0).handles);
    assert(lap_nfc_open() == LAP_RX_EUNSUPPORTED);
    assert(lap_nfc_next(0, NULL, NULL, 0) == LAP_RX_EUNSUPPORTED);
    assert(lap_nfc_write(0, NULL, 0) == LAP_RX_EUNSUPPORTED);
    assert(lap_mdns_browse(NULL, NULL) == LAP_IO_ENOTSUP);
    assert(lap_mdns_register(NULL) == LAP_IO_ENOTSUP);
    assert(lap_mdns_next(0, NULL) == LAP_IO_ENOTSUP);
    assert(!ls_lap_mdns_service());
    assert(!ls_lap_mdns_usage(0).bytes && !ls_lap_mdns_usage(0).handles);
    lap_rx_dsp_close(0); lap_nfc_close(0); lap_mdns_stop(0);
    ls_lap_api_callback(false);
    ls_lap_sys_end();
    for (int i=0; i<4; i++) ls_lap_sys_service();
    ls_lap_api_cleanup();
    puts("Disabled board services: exports, caps, unsupported errors, empty pools PASS");
    return 0;
}
