/* Physical transmitter protection. No legal/region/licence policy. */
#ifndef LS_TX_PROTECTION_H
#define LS_TX_PROTECTION_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { LS_TX_SOCKET, LS_TX_MIXRF, LS_TX_HACKRF, LS_TX_SIM, LS_TX_RADIOS };
/* Errors intentionally match LAP_IO_E*: power,duty,thermal,RX-only. */
int ls_tx_protect(unsigned radio, int dbm, bool rx_only, uint32_t airtime_us, uint64_t now_us);
void ls_tx_thermal(unsigned radio, bool hot); /* driver PA/over-temperature fault latch */
int ls_tx_last_error(unsigned radio);
void ls_tx_clear_error(unsigned radio);
/* Pure Semtech LoRa airtime; zero if configuration cannot be represented. */
uint32_t ls_tx_lora_airtime(unsigned bytes, unsigned sf, unsigned bw, unsigned cr,
                            unsigned preamble, bool crc, bool implicit);
#ifdef __cplusplus
}
#endif
#endif
