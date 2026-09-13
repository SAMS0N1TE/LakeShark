#ifndef LS_MIXRF_H
#define LS_MIXRF_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_MIXRF_CHANNELS 84
#define LS_MIXRF_CARD_HISTORY 32
typedef struct {
    bool ready,busy,keyboard,power,cc,nrf,nfc,receiving;
    uint8_t cc_version,nfc_identity,nrf_address_width;
    uint32_t frequency,samples;
    float rssi;
    bool scanning;
    bool scan_requested,receive_requested,nfc_requested,nfc_watching,nfc_field;
    uint8_t channel,occupancy[LS_MIXRF_CHANNELS];
    uint32_t sweeps,energy_hits;
    uint32_t nfc_samples,nfc_events;
    bool card_requested,card_scanning,card_present;
    uint16_t card_atqa;
    uint32_t card_polls,card_hits,card_tx,card_errors;
    /* Bounded probe history: raw AM RSSI, outcome 0 idle / 1 noise / 2 reply / 3 confirmed. */
    uint8_t card_level[LS_MIXRF_CARD_HISTORY],card_result[LS_MIXRF_CARD_HISTORY];
    uint8_t card_head,card_count;
    char status[80];
} ls_mixrf_status_t;
bool ls_mixrf_start(void);
void ls_mixrf_snapshot(ls_mixrf_status_t *out);
bool ls_mixrf_receive(bool on,uint32_t frequency);
bool ls_mixrf_scan(bool on);
bool ls_mixrf_nfc_watch(bool on);
bool ls_mixrf_card_scan(bool on);
void ls_mixrf_diagnostics(void);
#ifdef __cplusplus
}
#endif
#endif
