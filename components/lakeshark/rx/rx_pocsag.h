/* POCSAG as an RX HEAD plugin (ls_rx_plugin.h), one decoder for every
   receiver that hears pagers:
     DISC   the FM discriminator (FM on the SDR or the LR2021's I/Q), three
            timing loops at 512, 1200 and 2400 baud when the bound FM state
            says auto, else the one it names; the busiest synced one wins;
     BITS   clock-recovered FIFO bytes from the chip's FSK engine (LABS);
     BATCH  64-byte sync-stripped batches from the chip (LABS, PAGER RECON).
   BITS and BATCH carry their bit rate in the block, which picks the loop.

   Pages go to the bound FM state's page ring (none until bound) and to the
   decoded timeline; the FM state's pocsag_* counters follow the decoders, as
   the FM screen reads them. */
#ifndef RX_POCSAG_H
#define RX_POCSAG_H

#include "ls_rx_plugin.h"
#include "fm_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const ls_rx_plugin_t rx_pocsag;

/* Pages and counters into this FM state; NULL counts pages only. */
void rx_pocsag_bind(void *st, fm_state_t *out);
/* The lowest sync distance seen while hunting and how many came near. */
void rx_pocsag_near(const void *st, int *near_min, uint32_t *near);
uint32_t rx_pocsag_frames(const void *st);
uint32_t rx_pocsag_pages(const void *st);
/* A chip stream: make (or re-rate) its one loop now, at the chip's bit rate.
   Blocks carrying another rate re-rate it themselves. */
bool rx_pocsag_prepare(void *st, int baud);

#ifdef __cplusplus
}
#endif
#endif
