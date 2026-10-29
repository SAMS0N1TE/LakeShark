/* Host shim for test_fm_lr_runtime, which builds app_fm.c around stub
   decoders (pocsag_*, flex_* answer and do nothing there). The RX HEAD data
   plugins FM attaches are the same: present, and inert. */
#include "rx_pocsag.h"
#include "rx_fm_data.h"
#include <stddef.h>

static void inert(void *st, const ls_rx_block_t *b) { (void)st; (void)b; }

#define INERT_F(sym, nm, in, fr) const ls_rx_plugin_t sym = { .abi = LS_RX_ABI, .id = nm, .name = nm,     .doc = "host stub", .needs = { .inputs = (in), .front = (fr), .rsel_job = -1 }, .process = inert }
#define INERT(sym, nm, in) INERT_F(sym, nm, in, LS_RX_FRONT_NFM)

INERT(rx_pocsag, "POCSAG", LS_RX_DISC);
INERT(rx_flex, "FLEX", LS_RX_DISC);
INERT(rx_aprs, "APRS", LS_RX_PCM16);
INERT(rx_same, "SAME", LS_RX_PCM16);
INERT_F(rx_ais, "AIS", LS_RX_IQ_U8, LS_RX_FRONT_NONE);
INERT_F(rx_acars, "ACARS", LS_RX_ENV, LS_RX_FRONT_AM);

void rx_pocsag_bind(void *st, fm_state_t *out) { (void)st; (void)out; }
void rx_same_sink(same_callback_t fn, void *user) { (void)fn; (void)user; }
void rx_pocsag_near(const void *st, int *near_min, uint32_t *near) { (void)st; *near_min = 32; *near = 0; }
